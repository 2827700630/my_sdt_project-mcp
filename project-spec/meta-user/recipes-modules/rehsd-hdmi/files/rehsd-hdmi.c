// SPDX-License-Identifier: GPL-2.0
/*
* Adapted from Digilent, Author : Cosmin Tanislav <demonsingur@gmail.com>
*/

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_probe_helper.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <drm/drm_edid.h>
#include <linux/i2c.h>
// 新增缺失的平台设备头文件（解决编译报错核心）
#include <linux/platform_device.h>
#include <linux/module.h>

struct rehsd_hdmi {
    struct drm_encoder encoder;
    struct drm_connector connector;
    struct drm_device *drm_dev;

    struct device *dev;

    struct clk *clk;
    bool clk_enabled;

    struct i2c_adapter *i2c_bus;
    u32 fmax;
    u32 hmax;
    u32 vmax;
    u32 hpref;
    u32 vpref;
};

#define connector_to_hdmi(c) container_of(c, struct rehsd_hdmi, connector)
#define encoder_to_hdmi(e) container_of(e, struct rehsd_hdmi, encoder)

static int rehsd_hdmi_get_modes(struct drm_connector *connector)
{
    struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);
    struct edid *edid;
    int count = 0;

    if (hdmi->i2c_bus) {
        edid = drm_get_edid(connector, hdmi->i2c_bus);
        if (!edid) {
            dev_err(hdmi->dev, "failed to get edid data\n");
            return 0;
        }

        drm_connector_update_edid_property(connector, edid);
        count = drm_add_edid_modes(connector, edid);
        kfree(edid);
    } else {
        count = drm_add_modes_noedid(connector, hdmi->hmax, hdmi->vmax);
        drm_set_preferred_mode(connector, hdmi->hpref, hdmi->vpref);
    }

    // 修复：返回实际模式数量（原返回0逻辑错误）
    return count;
}

static int rehsd_hdmi_mode_valid(struct drm_connector *connector,  struct drm_display_mode *mode)
{
    struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);

    if (!mode)
        goto mode_bad;

    if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK
                        | DRM_MODE_FLAG_3D_MASK))
        goto mode_bad;

    if (mode->clock > hdmi->fmax
                    || mode->hdisplay > hdmi->hmax
                    || mode->vdisplay > hdmi->vmax)
        goto mode_bad;

    return MODE_OK;

mode_bad:
    return MODE_BAD;
}

static struct drm_encoder *rehsd_hdmi_best_encoder(struct drm_connector *connector)
{
    struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);
    return &hdmi->encoder;
}

static struct drm_connector_helper_funcs rehsd_hdmi_connector_helper_funcs = {
    .get_modes = rehsd_hdmi_get_modes,
    .mode_valid = rehsd_hdmi_mode_valid,
    .best_encoder = rehsd_hdmi_best_encoder,
};

static enum drm_connector_status rehsd_hdmi_detect(struct drm_connector *connector,
                    bool force)
{
    struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);

    if (!hdmi->i2c_bus)
        return connector_status_unknown;

    return drm_probe_ddc(hdmi->i2c_bus)
            ? connector_status_connected
            : connector_status_disconnected;
}

static void rehsd_hdmi_connector_destroy(struct drm_connector *connector)
{
    drm_connector_unregister(connector);
    drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs rehsd_hdmi_connector_funcs = {
    .detect = rehsd_hdmi_detect,
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = rehsd_hdmi_connector_destroy,
    .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
    .atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
    .reset = drm_atomic_helper_connector_reset,
};

static int rehsd_hdmi_create_connector(struct rehsd_hdmi *hdmi)
{
    struct drm_connector *connector = &hdmi->connector;
    struct drm_encoder *encoder = &hdmi->encoder;
    int ret;

    connector->polled = DRM_CONNECTOR_POLL_CONNECT
            | DRM_CONNECTOR_POLL_DISCONNECT;

    ret = drm_connector_init(hdmi->drm_dev, connector,
                    &rehsd_hdmi_connector_funcs,
                    DRM_MODE_CONNECTOR_HDMIA);
    if (ret) {
        dev_err(hdmi->dev, "failed to initialize connector\n");
        return ret;
    }
    drm_connector_helper_add(connector,
                    &rehsd_hdmi_connector_helper_funcs);

    drm_connector_register(connector);
    drm_connector_attach_encoder(connector, encoder);

    return 0;
}

static void rehsd_hdmi_atomic_mode_set(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state, struct drm_connector_state *connector_state)
{
    struct rehsd_hdmi *hdmi = encoder_to_hdmi(encoder);
    struct drm_display_mode *m = &crtc_state->adjusted_mode;
    // Clock rate is fixed at 74.25MHz for misc_clk_0, do not set
}

static void rehsd_hdmi_enable(struct drm_encoder *encoder)
{
    struct rehsd_hdmi *hdmi = encoder_to_hdmi(encoder);

    if (hdmi->clk_enabled)
        return;

    clk_prepare_enable(hdmi->clk);
    hdmi->clk_enabled = true;
}

static void rehsd_hdmi_disable(struct drm_encoder *encoder)
{
    // 移除无效的return语句（解决编译警告）
    struct rehsd_hdmi *hdmi = encoder_to_hdmi(encoder);

    if (!hdmi->clk_enabled)
        return;

    clk_disable_unprepare(hdmi->clk);
    hdmi->clk_enabled = false;
}

static const struct drm_encoder_helper_funcs rehsd_hdmi_encoder_helper_funcs = {
    .atomic_mode_set = rehsd_hdmi_atomic_mode_set,
    .enable = rehsd_hdmi_enable,
    .disable = rehsd_hdmi_disable,
};

static const struct drm_encoder_funcs rehsd_hdmi_encoder_funcs = {
    .destroy = drm_encoder_cleanup,
};

static int rehsd_hdmi_create_encoder(struct rehsd_hdmi *hdmi)
{
    struct drm_encoder *encoder = &hdmi->encoder;
    int ret;

    encoder->possible_crtcs = 1;
    ret = drm_encoder_init(hdmi->drm_dev, encoder,
                    &rehsd_hdmi_encoder_funcs,
                    DRM_MODE_ENCODER_TMDS, NULL);
    if (ret) {
        dev_err(hdmi->dev, "failed to initialize encoder\n");
        return ret;
    }
    drm_encoder_helper_add(encoder, &rehsd_hdmi_encoder_helper_funcs);

    return 0;
}

static int rehsd_hdmi_bind(struct device *dev, struct device *master,
                void *data)
{
    struct rehsd_hdmi *hdmi = dev_get_drvdata(dev);
    int ret;

    hdmi->drm_dev = data;

    ret = rehsd_hdmi_create_encoder(hdmi);
    if (ret) {
        dev_err(dev, "failed to create encoder: %d\n", ret);
        goto encoder_create_fail;
    }

    ret = rehsd_hdmi_create_connector(hdmi);
    if (ret) {
        dev_err(dev, "failed to create connector: %d\n", ret);
        goto hdmi_create_fail;
    }

    return 0;

hdmi_create_fail:
    drm_encoder_cleanup(&hdmi->encoder);
encoder_create_fail:
    return ret;
}

static void rehsd_hdmi_unbind(struct device *dev, struct device *master,
    void *data)
{
    struct rehsd_hdmi *hdmi = dev_get_drvdata(dev);

    rehsd_hdmi_disable(&hdmi->encoder);
}

static const struct component_ops rehsd_hdmi_component_ops = {
    .bind = rehsd_hdmi_bind,
    .unbind = rehsd_hdmi_unbind,
};

#define rehsd_ENC_MAX_FREQ 150000
#define rehsd_ENC_MAX_H 1280
#define rehsd_ENC_MAX_V 720
#define rehsd_ENC_PREF_H 1280
#define rehsd_ENC_PREF_V 720
static int rehsd_hdmi_parse_dt(struct rehsd_hdmi *hdmi)
{
    struct device *dev = hdmi->dev;
    int ret;

    hdmi->fmax = rehsd_ENC_MAX_FREQ;
    hdmi->hmax = rehsd_ENC_MAX_H;
    hdmi->vmax = rehsd_ENC_MAX_V;
    hdmi->hpref = rehsd_ENC_PREF_H;
    hdmi->vpref = rehsd_ENC_PREF_V;

    /* Enable clock parsing */
    hdmi->clk = devm_clk_get(dev, "clk");
    if (IS_ERR(hdmi->clk)) {
        ret = PTR_ERR(hdmi->clk);
        dev_err(dev, "failed to get hdmi clock: %d\n", ret);
        return ret;
    }

    /* I2C parsing remains commented as not needed */
    /*
    i2c_node = of_parse_phandle(node, "rehsd,edid-i2c", 0);
    if (i2c_node) {
        hdmi->i2c_bus = of_get_i2c_adapter_by_node(i2c_node);
        of_node_put(i2c_node);

        if (!hdmi->i2c_bus) {
            ret = -EPROBE_DEFER;
            dev_err(dev, "failed to get edid i2c adapter: %d\n", ret);
            return ret;
        }
    } else {
        dev_info(dev, "failed to find edid i2c property\n");
    }
    */

    return 0;
}

static int rehsd_hdmi_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct rehsd_hdmi *hdmi;
    int ret;

    hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
    if (!hdmi) {
        ret = -ENOMEM;
        dev_err(dev, "failed to allocate: %d\n", ret);
        return ret;
    }

    hdmi->dev = dev;

    ret = rehsd_hdmi_parse_dt(hdmi);
    if (ret) {
        dev_err(dev, "failed to parse device tree: %d\n", ret);
        return ret;
    }

    platform_set_drvdata(pdev, hdmi);

    ret = component_add(dev, &rehsd_hdmi_component_ops);
    if (ret < 0) {
        dev_err(dev, "fail to add component: %d\n", ret);
        return ret;
    }

    return 0;
}

// 修复：将remove函数返回值改为void（适配6.12内核）
static void rehsd_hdmi_remove(struct platform_device *pdev)
{
    struct rehsd_hdmi *hdmi = platform_get_drvdata(pdev);

    component_del(&pdev->dev, &rehsd_hdmi_component_ops);
    if (hdmi->i2c_bus)
        i2c_put_adapter(hdmi->i2c_bus);
    // 移除return语句，因为函数返回void
}

static const struct of_device_id rehsd_hdmi_of_match[] = {
    { .compatible = "rehsd,hdmi"},
    { }
};
MODULE_DEVICE_TABLE(of, rehsd_hdmi_of_match);

static struct platform_driver hdmi_driver = {
    .probe = rehsd_hdmi_probe,
    .remove = rehsd_hdmi_remove,
    .driver = {
            .name = "rehsd-hdmi",
            .of_match_table = rehsd_hdmi_of_match,
        },
};

module_platform_driver(hdmi_driver);

MODULE_AUTHOR("Cosmin Tanislav <demonsingur@gmail.com>");
MODULE_DESCRIPTION("rehsd FPGA HDMI driver");
MODULE_LICENSE("GPL v2");