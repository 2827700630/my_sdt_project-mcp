// SPDX-License-Identifier: GPL-2.0
/*
 * Digilent FPGA HDMI driver.
 *
 * Copyright (C) 2020 Digilent, Inc.
 *
 * Author : Cosmin Tanislav <demonsingur@gmail.com>
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_probe_helper.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>

struct digilent_hdmi
{
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

#define connector_to_hdmi(c) container_of(c, struct digilent_hdmi, connector)
#define encoder_to_hdmi(e) container_of(e, struct digilent_hdmi, encoder)

static int digilent_hdmi_get_modes(struct drm_connector *connector)
{
    struct digilent_hdmi *hdmi = connector_to_hdmi(connector);
    struct edid *edid;
    int count = 0;

    dev_info(hdmi->dev, "Getting modes for HDMI connector\n");

    if (hdmi->i2c_bus)
    {
        dev_info(hdmi->dev, "Using I2C for EDID\n");
        edid = drm_get_edid(connector, hdmi->i2c_bus);
        if (!edid)
        {
            dev_err(hdmi->dev, "failed to get edid data\n");
            return 0;
        }

        drm_connector_update_edid_property(connector, edid);
        count = drm_add_edid_modes(connector, edid);
        kfree(edid);
        dev_info(hdmi->dev, "Added %d EDID modes\n", count);
    }
    else
    {
        dev_info(hdmi->dev, "No I2C, using no-EDID modes: hmax=%u, vmax=%u\n", hdmi->hmax, hdmi->vmax);
        count = drm_add_modes_noedid(connector, hdmi->hmax, hdmi->vmax);
        drm_set_preferred_mode(connector, hdmi->hpref, hdmi->vpref);
        dev_info(hdmi->dev, "Set preferred mode: %ux%u, added %d modes\n", hdmi->hpref, hdmi->vpref, count);
    }

    return count;
}

static int digilent_hdmi_mode_valid(struct drm_connector *connector,
                                    struct drm_display_mode *mode)
{
    struct digilent_hdmi *hdmi = connector_to_hdmi(connector);

    if (!mode)
        goto mode_bad;

    if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_3D_MASK))
        goto mode_bad;

    if (mode->clock > hdmi->fmax || mode->hdisplay > hdmi->hmax || mode->vdisplay > hdmi->vmax)
        goto mode_bad;

    return MODE_OK;

mode_bad:
    return MODE_BAD;
}

static struct drm_encoder *digilent_hdmi_best_encoder(struct drm_connector *connector)
{
    struct digilent_hdmi *hdmi = connector_to_hdmi(connector);
    return &hdmi->encoder;
}

static struct drm_connector_helper_funcs digilent_hdmi_connector_helper_funcs = {
    .get_modes = digilent_hdmi_get_modes,
    .mode_valid = digilent_hdmi_mode_valid,
    .best_encoder = digilent_hdmi_best_encoder,
};

static enum drm_connector_status digilent_hdmi_detect(struct drm_connector *connector, bool force)
{
    struct digilent_hdmi *hdmi = connector_to_hdmi(connector);
    struct device_node *node = hdmi->dev->of_node;
    enum drm_connector_status status;

    dev_info(hdmi->dev, "Detecting HDMI connector, force=%d\n", force);

    // 检查设备树属性，强制连接
    if (of_property_read_bool(node, "hdmi,force-hot-plug"))
    {
        dev_info(hdmi->dev, "Force hot-plug enabled, status=connected\n");
        return connector_status_connected;
    }

    if (!hdmi->i2c_bus)
    {
        dev_info(hdmi->dev, "No I2C bus, status=unknown\n");
        return connector_status_unknown;
    }

    status = drm_probe_ddc(hdmi->i2c_bus) ? connector_status_connected : connector_status_disconnected;
    dev_info(hdmi->dev, "I2C probe result: status=%s\n", status == connector_status_connected ? "connected" : "disconnected");
    return status;
}

static void digilent_hdmi_connector_destroy(struct drm_connector *connector)
{
    drm_connector_unregister(connector);
    drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs digilent_hdmi_connector_funcs = {
    .detect = digilent_hdmi_detect,
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = digilent_hdmi_connector_destroy,
    .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
    .atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
    .reset = drm_atomic_helper_connector_reset,
};

static int digilent_hdmi_create_connector(struct digilent_hdmi *hdmi)
{
    struct drm_connector *connector = &hdmi->connector;
    struct drm_encoder *encoder = &hdmi->encoder;
    int ret;

    connector->polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

    ret = drm_connector_init(hdmi->drm_dev, connector,
                             &digilent_hdmi_connector_funcs,
                             DRM_MODE_CONNECTOR_HDMIA);
    if (ret)
    {
        dev_err(hdmi->dev, "failed to initialize connector\n");
        return ret;
    }
    drm_connector_helper_add(connector,
                             &digilent_hdmi_connector_helper_funcs);

    drm_connector_register(connector);
    drm_connector_attach_encoder(connector, encoder);

    return 0;
}

static void digilent_hdmi_atomic_mode_set(struct drm_encoder *encoder,
                                          struct drm_crtc_state *crtc_state,
                                          struct drm_connector_state *connector_state)
{
    struct digilent_hdmi *hdmi = encoder_to_hdmi(encoder);
    struct drm_display_mode *m = &crtc_state->adjusted_mode;
    unsigned long rate = m->clock * 1000;

    dev_info(hdmi->dev, "Setting mode: %ux%u @ %u Hz, setting clock to %lu Hz\n",
             m->hdisplay, m->vdisplay, drm_mode_vrefresh(m), rate);
    clk_set_rate(hdmi->clk, rate);
}

static void digilent_hdmi_enable(struct drm_encoder *encoder)
{
    struct digilent_hdmi *hdmi = encoder_to_hdmi(encoder);

    if (hdmi->clk_enabled)
    {
        dev_info(hdmi->dev, "Clock already enabled\n");
        return;
    }

    dev_info(hdmi->dev, "Enabling HDMI clock\n");
    clk_prepare_enable(hdmi->clk);
    hdmi->clk_enabled = true;
}

static void digilent_hdmi_disable(struct drm_encoder *encoder)
{
    struct digilent_hdmi *hdmi = encoder_to_hdmi(encoder);

    if (!hdmi->clk_enabled)
    {
        dev_info(hdmi->dev, "Clock already disabled\n");
        return;
    }

    dev_info(hdmi->dev, "Disabling HDMI clock\n");
    clk_disable_unprepare(hdmi->clk);
    hdmi->clk_enabled = false;
}

static const struct drm_encoder_helper_funcs digilent_hdmi_encoder_helper_funcs = {
    .atomic_mode_set = digilent_hdmi_atomic_mode_set,
    .enable = digilent_hdmi_enable,
    .disable = digilent_hdmi_disable,
};

static const struct drm_encoder_funcs digilent_hdmi_encoder_funcs = {
    .destroy = drm_encoder_cleanup,
};

static int digilent_hdmi_create_encoder(struct digilent_hdmi *hdmi)
{
    struct drm_encoder *encoder = &hdmi->encoder;
    int ret;

    encoder->possible_crtcs = 1;
    ret = drm_encoder_init(hdmi->drm_dev, encoder,
                           &digilent_hdmi_encoder_funcs,
                           DRM_MODE_ENCODER_TMDS, NULL);
    if (ret)
    {
        dev_err(hdmi->dev, "failed to initialize encoder\n");
        return ret;
    }
    drm_encoder_helper_add(encoder, &digilent_hdmi_encoder_helper_funcs);

    return 0;
}

static int digilent_hdmi_bind(struct device *dev, struct device *master,
                              void *data)
{
    struct digilent_hdmi *hdmi = dev_get_drvdata(dev);
    int ret;

    dev_info(dev, "Binding HDMI to DRM\n");
    hdmi->drm_dev = data;

    ret = digilent_hdmi_create_encoder(hdmi);
    if (ret)
    {
        dev_err(dev, "failed to create encoder: %d\n", ret);
        goto encoder_create_fail;
    }
    dev_info(dev, "Encoder created successfully\n");

    ret = digilent_hdmi_create_connector(hdmi);
    if (ret)
    {
        dev_err(dev, "failed to create connector: %d\n", ret);
        goto hdmi_create_fail;
    }
    dev_info(dev, "Connector created successfully\n");

    return 0;

hdmi_create_fail:
    drm_encoder_cleanup(&hdmi->encoder);
encoder_create_fail:
    return ret;
}

static void digilent_hdmi_unbind(struct device *dev, struct device *master,
                                 void *data)
{
    struct digilent_hdmi *hdmi = dev_get_drvdata(dev);

    digilent_hdmi_disable(&hdmi->encoder);
}

static const struct component_ops digilent_hdmi_component_ops = {
    .bind = digilent_hdmi_bind,
    .unbind = digilent_hdmi_unbind,
};

#define DIGILENT_ENC_MAX_FREQ 150000
#define DIGILENT_ENC_MAX_H 1920
#define DIGILENT_ENC_MAX_V 1080
#define DIGILENT_ENC_PREF_H 1280
#define DIGILENT_ENC_PREF_V 720
static int digilent_hdmi_parse_dt(struct digilent_hdmi *hdmi)
{
    struct device *dev = hdmi->dev;
    struct device_node *node = dev->of_node;
    struct device_node *i2c_node;
    int ret;

    hdmi->clk = devm_clk_get(dev, "clk");
    if (IS_ERR(hdmi->clk))
    {
        ret = PTR_ERR(hdmi->clk);
        dev_err(dev, "failed to get hdmi clock: %d\n", ret);
        return ret;
    }

    i2c_node = of_parse_phandle(node, "digilent,edid-i2c", 0);
    if (i2c_node)
    {
        hdmi->i2c_bus = of_get_i2c_adapter_by_node(i2c_node);
        of_node_put(i2c_node);

        if (!hdmi->i2c_bus)
        {
            ret = -EPROBE_DEFER;
            dev_err(dev, "failed to get edid i2c adapter: %d\n", ret);
            return ret;
        }
        dev_info(dev, "I2C bus found for EDID\n");
    }
    else
    {
        dev_info(dev, "No I2C property found, will use force-hot-plug or no-EDID modes\n");
    }

    ret = of_property_read_u32(node, "digilent,fmax", &hdmi->fmax);
    if (ret < 0)
        hdmi->fmax = DIGILENT_ENC_MAX_FREQ;

    ret = of_property_read_u32(node, "digilent,hmax", &hdmi->hmax);
    if (ret < 0)
        hdmi->hmax = DIGILENT_ENC_MAX_H;

    ret = of_property_read_u32(node, "digilent,vmax", &hdmi->vmax);
    if (ret < 0)
        hdmi->vmax = DIGILENT_ENC_MAX_V;

    ret = of_property_read_u32(node, "digilent,hpref", &hdmi->hpref);
    if (ret < 0)
        hdmi->hpref = DIGILENT_ENC_PREF_H;

    ret = of_property_read_u32(node, "digilent,vpref", &hdmi->vpref);
    if (ret < 0)
        hdmi->vpref = DIGILENT_ENC_PREF_V;

    return 0;
}

static int digilent_hdmi_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct digilent_hdmi *hdmi;
    int ret;

    dev_info(dev, "Probing Digilent HDMI driver\n");

    hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
    if (!hdmi)
    {
        ret = -ENOMEM;
        dev_err(dev, "failed to allocate: %d\n", ret);
        return ret;
    }

    hdmi->dev = dev;

    ret = digilent_hdmi_parse_dt(hdmi);
    if (ret)
    {
        dev_err(dev, "failed to parse device tree: %d\n", ret);
        return ret;
    }

    platform_set_drvdata(pdev, hdmi);

    ret = component_add(dev, &digilent_hdmi_component_ops);
    if (ret < 0)
    {
        dev_err(dev, "fail to add component: %d\n", ret);
        return ret;
    }

    dev_info(dev, "HDMI probe successful\n");
    return 0;
}

static void digilent_hdmi_remove(struct platform_device *pdev)
{
    struct digilent_hdmi *hdmi = platform_get_drvdata(pdev);

    component_del(&pdev->dev, &digilent_hdmi_component_ops);
    if (hdmi->i2c_bus)
        i2c_put_adapter(hdmi->i2c_bus);
}

static const struct of_device_id digilent_hdmi_of_match[] = {
    {.compatible = "digilent,hdmi"},
    {}};
MODULE_DEVICE_TABLE(of, digilent_hdmi_of_match);

static struct platform_driver hdmi_driver = {
    .probe = digilent_hdmi_probe,
    .remove = digilent_hdmi_remove,
    .driver = {
        .name = "digilent-hdmi",
        .of_match_table = digilent_hdmi_of_match,
    },
};

module_platform_driver(hdmi_driver);

MODULE_AUTHOR("Cosmin Tanislav <demonsingur@gmail.com>");
MODULE_DESCRIPTION("Digilent FPGA HDMI driver");
MODULE_LICENSE("GPL v2");
