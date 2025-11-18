#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <driver/hwmon/hwmon.h>

#define DT_DRV_COMPAT linkedsemi_hwmon
#define HWMON_INIT_PRIORITY 91

static int hwmon_ls_init(const struct device *dev)
{
    const struct hwmon_ls_config *cfg = dev->config;
    
    if (cfg == NULL) {
        return -EINVAL;
    }

    return 0;
}

const struct hwmon_ls_config *hwmon_ls_get_config(const struct device *dev)
{
    if (!dev || !dev->config) {
        return NULL;
    }
    return (const struct hwmon_ls_config *)dev->config;
}

#define LS_HWMON_IN_INIT(node,prop,idx) \
	{\
		.dev = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node,hwmon_ins,idx)),\
		.channel = DT_PHA_BY_IDX(node,hwmon_ins,idx,channel)\
	},

#define LS_HWMON_OUT_INIT(node,prop,idx) \
	{\
		.dev = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node,hwmon_outs,idx)),\
		.channel = DT_PHA_BY_IDX(node,hwmon_outs,idx,channel)\
	},

#define LS_HWMON_INIT(idx)\
	IF_ENABLED(DT_INST_NODE_HAS_PROP(idx,hwmon_ins),(\
		static const struct hwmon_ref hwmon_ins_##idx[] = {\
			DT_INST_FOREACH_PROP_ELEM(idx,hwmon_ins,LS_HWMON_IN_INIT)\
		};\
	))\
	IF_ENABLED(DT_INST_NODE_HAS_PROP(idx,hwmon_outs),(\
		static const struct hwmon_ref hwmon_outs_##idx[] = {\
			DT_INST_FOREACH_PROP_ELEM(idx,hwmon_outs,LS_HWMON_OUT_INIT)\
		};\
	))\
	static const struct hwmon_ls_config hwmon_ls_cfg_##idx = {\
		IF_ENABLED(DT_INST_NODE_HAS_PROP(idx,hwmon_ins),(\
			.in = hwmon_ins_##idx,\
			.in_num = ARRAY_SIZE(hwmon_ins_##idx),\
		))\
		IF_ENABLED(DT_INST_NODE_HAS_PROP(idx,hwmon_outs),(\
			.out = hwmon_outs_##idx,\
			.out_num = ARRAY_SIZE(hwmon_outs_##idx),\
		))\
		.class = DT_INST_PROP(idx,class),\
	};\
	DEVICE_DT_INST_DEFINE(idx,hwmon_ls_init,NULL,NULL,\
		&hwmon_ls_cfg_##idx,POST_KERNEL,HWMON_INIT_PRIORITY,NULL);

DT_INST_FOREACH_STATUS_OKAY(LS_HWMON_INIT)