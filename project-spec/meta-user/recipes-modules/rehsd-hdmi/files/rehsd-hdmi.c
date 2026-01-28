// SPDX-License-Identifier: GPL-2.0
/*
 * Adapted from Digilent, Author : Cosmin Tanislav <demonsingur@gmail.com>
 * Modified: Add debug logs for error locating
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

#include <linux/platform_device.h>

struct rehsd_hdmi
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

#define connector_to_hdmi(c) container_of(c, struct rehsd_hdmi, connector)
#define encoder_to_hdmi(e) container_of(e, struct rehsd_hdmi, encoder)

static int rehsd_hdmi_get_modes(struct drm_connector *connector)
{
	struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);
	struct edid *edid;
	int count = 0;

	dev_dbg(hdmi->dev, "[%s] Enter get_modes, i2c_bus=%p\n", __func__, hdmi->i2c_bus);

	if (hdmi->i2c_bus)
	{
		edid = drm_get_edid(connector, hdmi->i2c_bus);
		if (!edid)
		{
			dev_err(hdmi->dev, "[%s] Failed to get EDID data from i2c bus\n", __func__);
			return 0;
		}

		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, edid);
		dev_dbg(hdmi->dev, "[%s] Got EDID, added %d modes\n", __func__, count);
		kfree(edid);
	}
	else
	{
		count = drm_add_modes_noedid(connector, hdmi->hmax, hdmi->vmax);
		drm_set_preferred_mode(connector, hdmi->hpref, hdmi->vpref);
		dev_dbg(hdmi->dev, "[%s] No i2c bus, add no-edid modes (hmax=%d, vmax=%d), pref(%d,%d), count=%d\n",
				__func__, hdmi->hmax, hdmi->vmax, hdmi->hpref, hdmi->vpref, count);
	}

	// 原代码返回0是错误的？这里保留原逻辑但添加调试
	dev_dbg(hdmi->dev, "[%s] Exit, return count=%d (NOTE: original code returns 0)\n", __func__, count);
	return count;
}

static int rehsd_hdmi_mode_valid(struct drm_connector *connector, struct drm_display_mode *mode)
{
	struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);

	dev_dbg(hdmi->dev, "[%s] Enter, mode=%p\n", __func__, mode);

	if (!mode)
	{
		dev_dbg(hdmi->dev, "[%s] Mode is NULL, return MODE_BAD\n", __func__);
		goto mode_bad;
	}

	dev_dbg(hdmi->dev, "[%s] Mode info: clock=%d, hdisplay=%d, vdisplay=%d, flags=0x%x\n",
			__func__, mode->clock, mode->hdisplay, mode->vdisplay, mode->flags);

	if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_3D_MASK))
	{
		dev_dbg(hdmi->dev, "[%s] Mode has invalid flags (interlace/dblclk/3d), return MODE_BAD\n", __func__);
		goto mode_bad;
	}

	if (mode->clock > hdmi->fmax || mode->hdisplay > hdmi->hmax || mode->vdisplay > hdmi->vmax)
	{
		dev_dbg(hdmi->dev, "[%s] Mode out of limit: clock(%d>%d) OR hdisplay(%d>%d) OR vdisplay(%d>%d), return MODE_BAD\n",
				__func__, mode->clock, hdmi->fmax, mode->hdisplay, hdmi->hmax, mode->vdisplay, hdmi->vmax);
		goto mode_bad;
	}

	dev_dbg(hdmi->dev, "[%s] Mode is valid, return MODE_OK\n", __func__);
	return MODE_OK;

mode_bad:
	return MODE_BAD;
}

static struct drm_encoder *rehsd_hdmi_best_encoder(struct drm_connector *connector)
{
	struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);
	dev_dbg(hdmi->dev, "[%s] Return encoder=%p\n", __func__, &hdmi->encoder);
	return &hdmi->encoder;
}

static struct drm_connector_helper_funcs rehsd_hdmi_connector_helper_funcs = {
	.get_modes = rehsd_hdmi_get_modes,
	.mode_valid = rehsd_hdmi_mode_valid,
	.best_encoder = rehsd_hdmi_best_encoder,
};

static enum drm_connector_status rehsd_hdmi_detect(struct drm_connector *connector, bool force)
{
	struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);
	enum drm_connector_status status;

	dev_dbg(hdmi->dev, "[%s] Enter, force=%d, i2c_bus=%p\n", __func__, force, hdmi->i2c_bus);

	if (!hdmi->i2c_bus)
	{
		dev_dbg(hdmi->dev, "[%s] No i2c bus, return connector_status_unknown\n", __func__);
		return connector_status_unknown;
	}

	status = drm_probe_ddc(hdmi->i2c_bus) ? connector_status_connected : connector_status_disconnected;
	dev_dbg(hdmi->dev, "[%s] DDC probe result: %s\n", __func__,
			status == connector_status_connected ? "connected" : "disconnected");

	return status;
}

static void rehsd_hdmi_connector_destroy(struct drm_connector *connector)
{
	struct rehsd_hdmi *hdmi = connector_to_hdmi(connector);
	dev_dbg(hdmi->dev, "[%s] Enter, connector=%p\n", __func__, connector);

	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);

	dev_dbg(hdmi->dev, "[%s] Connector destroyed\n", __func__);
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

	dev_dbg(hdmi->dev, "[%s] Enter, connector=%p, encoder=%p\n", __func__, connector, encoder);

	connector->polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_connector_init(hdmi->drm_dev, connector,
							 &rehsd_hdmi_connector_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
	{
		dev_err(hdmi->dev, "[%s] Failed to initialize connector, ret=%d\n", __func__, ret);
		return ret;
	}

	drm_connector_helper_add(connector, &rehsd_hdmi_connector_helper_funcs);
	drm_connector_register(connector);
	drm_connector_attach_encoder(connector, encoder);

	dev_dbg(hdmi->dev, "[%s] Connector created and attached to encoder successfully\n", __func__);
	return 0;
}

static void rehsd_hdmi_atomic_mode_set(struct drm_encoder *encoder,
									   struct drm_crtc_state *crtc_state, struct drm_connector_state *connector_state)
{
	struct rehsd_hdmi *hdmi = encoder_to_hdmi(encoder);
	struct drm_display_mode *m = &crtc_state->adjusted_mode;
	unsigned long target_rate = m->clock * 1000;
	printk(KERN_INFO "HDMI: m->clock = %lu, target_rate = %lu (expected: 74250000)\n", 
       m->clock, target_rate);

	int ret;

	dev_dbg(hdmi->dev, "[%s] Enter, encoder=%p, target clock=%d KHz (raw=%lu Hz)\n",
			__func__, encoder, m->clock, target_rate);

	if (IS_ERR_OR_NULL(hdmi->clk))
	{
		dev_err(hdmi->dev, "[%s] HDMI clk is invalid (clk=%p)\n", __func__, hdmi->clk);
		return;
	}

	//ret = clk_set_rate(hdmi->clk, target_rate);
	if (ret)
	{
		dev_err(hdmi->dev, "[%s] Failed to set clk rate to %lu Hz, ret=%d\n",
				__func__, target_rate, ret);
	}
	else
	{
		dev_dbg(hdmi->dev, "[%s] Clk rate set to %lu Hz (actual: %lu Hz)\n",
				__func__, target_rate, clk_get_rate(hdmi->clk));
	}
}

static void rehsd_hdmi_enable(struct drm_encoder *encoder)
{
	struct rehsd_hdmi *hdmi = encoder_to_hdmi(encoder);
	dev_dbg(hdmi->dev, "[%s] Enter, encoder=%p, clk_enabled=%d\n",
			__func__, encoder, hdmi->clk_enabled);

	if (hdmi->clk_enabled)
	{
		dev_dbg(hdmi->dev, "[%s] Clk already enabled, skip\n", __func__);
		return;
	}

	clk_prepare_enable(hdmi->clk);
	hdmi->clk_enabled = true;
	dev_dbg(hdmi->dev, "[%s] Clk enabled successfully\n", __func__);

	return;
}

static void rehsd_hdmi_disable(struct drm_encoder *encoder)
{
	struct rehsd_hdmi *hdmi = encoder_to_hdmi(encoder);
	dev_dbg(hdmi->dev, "[%s] Enter, encoder=%p, clk_enabled=%d\n",
			__func__, encoder, hdmi->clk_enabled);

	if (!hdmi->clk_enabled)
	{
		dev_dbg(hdmi->dev, "[%s] Clk already disabled, skip\n", __func__);
		return;
	}

	clk_disable_unprepare(hdmi->clk);
	hdmi->clk_enabled = false;
	dev_dbg(hdmi->dev, "[%s] Clk disabled successfully\n", __func__);

	return;
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

	dev_dbg(hdmi->dev, "[%s] Enter, encoder=%p\n", __func__, encoder);

	encoder->possible_crtcs = 1;
	ret = drm_encoder_init(hdmi->drm_dev, encoder,
						   &rehsd_hdmi_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
	{
		dev_err(hdmi->dev, "[%s] Failed to initialize encoder, ret=%d\n", __func__, ret);
		return ret;
	}

	drm_encoder_helper_add(encoder, &rehsd_hdmi_encoder_helper_funcs);
	dev_dbg(hdmi->dev, "[%s] Encoder created successfully\n", __func__);

	return 0;
}

static int rehsd_hdmi_bind(struct device *dev, struct device *master, void *data)
{
	struct rehsd_hdmi *hdmi = dev_get_drvdata(dev);
	int ret;

	dev_dbg(dev, "[%s] Enter, dev=%p, master=%p, drm_dev=%p\n", __func__, dev, master, data);

	hdmi->drm_dev = data;

	ret = rehsd_hdmi_create_encoder(hdmi);
	if (ret)
	{
		dev_err(dev, "[%s] Failed to create encoder, ret=%d\n", __func__, ret);
		goto encoder_create_fail;
	}

	ret = rehsd_hdmi_create_connector(hdmi);
	if (ret)
	{
		dev_err(dev, "[%s] Failed to create connector, ret=%d\n", __func__, ret);
		goto hdmi_create_fail;
	}

	dev_dbg(dev, "[%s] Bind success\n", __func__);
	return 0;

hdmi_create_fail:
	drm_encoder_cleanup(&hdmi->encoder);
	dev_dbg(dev, "[%s] Cleanup encoder after connector create fail\n", __func__);
encoder_create_fail:
	return ret;
}

static void rehsd_hdmi_unbind(struct device *dev, struct device *master, void *data)
{
	struct rehsd_hdmi *hdmi = dev_get_drvdata(dev);

	dev_dbg(dev, "[%s] Enter, dev=%p, master=%p\n", __func__, dev, master);

	rehsd_hdmi_disable(&hdmi->encoder);
	dev_dbg(dev, "[%s] Unbind success\n", __func__);
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
	struct device_node *node = dev->of_node;
	struct device_node *i2c_node;
	int ret;

	dev_dbg(dev, "[%s] Enter, node=%p\n", __func__, node);

	// 默认值
	hdmi->fmax = rehsd_ENC_MAX_FREQ;
	hdmi->hmax = rehsd_ENC_MAX_H;
	hdmi->vmax = rehsd_ENC_MAX_V;
	hdmi->hpref = rehsd_ENC_PREF_H;
	hdmi->vpref = rehsd_ENC_PREF_V;

	dev_dbg(dev, "[%s] Default params: fmax=%d, hmax=%d, vmax=%d, hpref=%d, vpref=%d\n",
			__func__, hdmi->fmax, hdmi->hmax, hdmi->vmax, hdmi->hpref, hdmi->vpref);

	hdmi->clk = devm_clk_get(dev, "clk");
	if (IS_ERR(hdmi->clk))
	{
		ret = PTR_ERR(hdmi->clk);
		dev_err(dev, "[%s] Failed to get hdmi clock, ret=%d\n", __func__, ret);
		return ret;
	}
	dev_dbg(dev, "[%s] Got clk=%p\n", __func__, hdmi->clk);

	/*
	i2c_node = of_parse_phandle(node, "rehsd,edid-i2c", 0);
	if (i2c_node)
	{
		hdmi->i2c_bus = of_get_i2c_adapter_by_node(i2c_node);
		of_node_put(i2c_node);
		dev_dbg(dev, "[%s] Parsed edid-i2c phandle, i2c_bus=%p\n", __func__, hdmi->i2c_bus);

		if (!hdmi->i2c_bus)
		{
			ret = -EPROBE_DEFER;
			dev_err(dev, "[%s] Failed to get edid i2c adapter, ret=%d\n", __func__, ret);
			return ret;
		}
	}
	else
	{
		dev_info(dev, "[%s] Failed to find edid i2c property\n", __func__);
	}

	ret = of_property_read_u32(node, "rehsd,fmax", &hdmi->fmax);
	if (ret < 0)
	{
		dev_dbg(dev, "[%s] No rehsd,fmax property, use default=%d\n", __func__, rehsd_ENC_MAX_FREQ);
		hdmi->fmax = rehsd_ENC_MAX_FREQ;
	}
	else
	{
		dev_dbg(dev, "[%s] Read rehsd,fmax=%d\n", __func__, hdmi->fmax);
	}

	ret = of_property_read_u32(node, "rehsd,hmax", &hdmi->hmax);
	if (ret < 0)
	{
		dev_dbg(dev, "[%s] No rehsd,hmax property, use default=%d\n", __func__, rehsd_ENC_MAX_H);
		hdmi->hmax = rehsd_ENC_MAX_H;
	}
	else
	{
		dev_dbg(dev, "[%s] Read rehsd,hmax=%d\n", __func__, hdmi->hmax);
	}

	ret = of_property_read_u32(node, "rehsd,vmax", &hdmi->vmax);
	if (ret < 0)
	{
		dev_dbg(dev, "[%s] No rehsd,vmax property, use default=%d\n", __func__, rehsd_ENC_MAX_V);
		hdmi->vmax = rehsd_ENC_MAX_V;
	}
	else
	{
		dev_dbg(dev, "[%s] Read rehsd,vmax=%d\n", __func__, hdmi->vmax);
	}

	ret = of_property_read_u32(node, "rehsd,hpref", &hdmi->hpref);
	if (ret < 0)
	{
		dev_dbg(dev, "[%s] No rehsd,hpref property, use default=%d\n", __func__, rehsd_ENC_PREF_H);
		hdmi->hpref = rehsd_ENC_PREF_H;
	}
	else
	{
		dev_dbg(dev, "[%s] Read rehsd,hpref=%d\n", __func__, hdmi->hpref);
	}

	ret = of_property_read_u32(node, "rehsd,vpref", &hdmi->vpref);
	if (ret < 0)
	{
		dev_dbg(dev, "[%s] No rehsd,vpref property, use default=%d\n", __func__, rehsd_ENC_PREF_V);
		hdmi->vpref = rehsd_ENC_PREF_V;
	}
	else
	{
		dev_dbg(dev, "[%s] Read rehsd,vpref=%d\n", __func__, hdmi->vpref);
	}
	*/

	dev_dbg(dev, "[%s] Final params: fmax=%d, hmax=%d, vmax=%d, hpref=%d, vpref=%d, i2c_bus=%p, clk=%p\n",
			__func__, hdmi->fmax, hdmi->hmax, hdmi->vmax, hdmi->hpref, hdmi->vpref,
			hdmi->i2c_bus, hdmi->clk);

	return 0;
}

static int rehsd_hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rehsd_hdmi *hdmi;
	int ret;

	dev_dbg(dev, "[%s] Enter, pdev=%p\n", __func__, pdev);

	hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
	{
		ret = -ENOMEM;
		dev_err(dev, "[%s] Failed to allocate hdmi struct, ret=%d\n", __func__, ret);
		return ret;
	}
	dev_dbg(dev, "[%s] Allocated hdmi struct at %p\n", __func__, hdmi);

	hdmi->dev = dev;

	ret = rehsd_hdmi_parse_dt(hdmi);
	if (ret)
	{
		dev_err(dev, "[%s] Failed to parse device tree, ret=%d\n", __func__, ret);
		return ret;
	}
	dev_dbg(dev, "[%s] Parse device tree success\n", __func__);

	platform_set_drvdata(pdev, hdmi);
	dev_dbg(dev, "[%s] Set drvdata to %p\n", __func__, hdmi);

	ret = component_add(dev, &rehsd_hdmi_component_ops);
	if (ret < 0)
	{
		dev_err(dev, "[%s] Failed to add component, ret=%d\n", __func__, ret);
		return ret;
	}
	dev_dbg(dev, "[%s] Add component success, ret=%d\n", __func__, ret);

	dev_dbg(dev, "[%s] Probe success\n", __func__);
	return 0;
}

static void rehsd_hdmi_remove(struct platform_device *pdev)
{
	struct rehsd_hdmi *hdmi = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	dev_dbg(dev, "[%s] Enter, pdev=%p, hdmi=%p\n", __func__, pdev, hdmi);

	component_del(&pdev->dev, &rehsd_hdmi_component_ops);
	dev_dbg(dev, "[%s] Removed component\n", __func__);

	if (hdmi->i2c_bus)
	{
		i2c_put_adapter(hdmi->i2c_bus);
		dev_dbg(dev, "[%s] Put i2c adapter %p\n", __func__, hdmi->i2c_bus);
	}

	dev_dbg(dev, "[%s] Remove success\n", __func__);
}

static const struct of_device_id rehsd_hdmi_of_match[] = {
	{.compatible = "rehsd,hdmi"},
	{}};
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
MODULE_DESCRIPTION("rehsd FPGA HDMI driver (with debug logs)");
MODULE_LICENSE("GPL v2");