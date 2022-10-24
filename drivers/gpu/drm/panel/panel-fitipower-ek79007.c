#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_mode.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

enum ek79007_op {
    EK79007_SWITCH_PAGE,
    EK79007_COMMAND
};

struct ek79007_instr {
    enum ek79007_op op;

    union arg {
        struct cmd {
            u8 cmd;
            u8 data;
        }cmd;
        u8 page;        
    } arg;
};

struct ek79007_desc {
    const struct ek79007_instr      *init;
    const size_t                    init_length;
    const struct drm_display_mode   *mode;    
};

struct ek79007 {
    struct drm_panel            panel;
    struct mipi_display_device  *dsi;
    const struct ek79007_desc   *desc;

    struct regulator            *power;
    struct gpio_desc            *reset;
};

#define EK79007_SWITCH_PAGE_INSTR(_page)    \
        {                                   \
            .op  = EK79007_SWITCH_PAGE,     \
            .arg = {                        \
                    .page = (_page),        \
            },                              \
        }                                   \

#define EK79007_COMMAND_INSTR(_cmd, _data)   \
        {                                    \
            .op  = EK79007_COMMAND,          \
            .arg = {                         \
                    .cmd = {                 \
                            .cmd = (_cmd),   \
                            .data = (_data), \
                    },                       \
            },                               \
        }                                    \

static const struct ek79007_instr LX700B4008CTP14_init[] = {
    EK79007_COMMAND_INSTR(0x80, 0x8b),  //Gamma Color Register
    EK79007_COMMAND_INSTR(0x81, 0x78),  //Gamma Color Register
    EK79007_COMMAND_INSTR(0x82, 0x84),  //Gamma Color Register
    EK79007_COMMAND_INSTR(0x83, 0x88),  //Gamma Color Register
    EK79007_COMMAND_INSTR(0x84, 0xa8),  //Gamma Color Register
    EK79007_COMMAND_INSTR(0x85, 0xe3),  //Gamma Color Register
    EK79007_COMMAND_INSTR(0x86, 0x88),  //Gamma Color Register
    EK79007_COMMAND_INSTR(0xb2, 0x10),  //Panel Control Register 0x10 - 2 lane MIPI, 0x20 - 3 lane MIPI, 0x30 - 4 lane MIPI
}

static inline struct ek79007 *panel_to_ek79007(struct drm_panel *panel){
    return container_of(panel, struct ek79007, panel);
}

//probably this function will not be used.... 
static int ek79007_switch_page(struct ek79007 *ctx, u8 page){
    u8 buff[4] = {0x00, 0x00, 0x00, page};
    int ret;

    ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buff, sizeof(buff));
    
    if (ret < 0)
        return ret;

    return 0; 
}

static int ek79007_send_cmd_data(struct ek79007 *ctx, u8 cmd, u8 data){
    u8 buff[2] = {cmd, data};
    int ret;

    ret = mipi_dsi_dcs_write_buffer(ctx->dsi, buff, sizeof(buff));

    if (ret < 0)
        return ret;

    return 0; 
}

static int ek79007_prepare(struct drm_panel *panel){
    struct ek79007 *ctx = panel_to_ek79007(panel);
    unsigned int i;
    int ret;

    ret = regulator_enable(ctx->power);
    if (ret)
        return ret;

    msleep(5);

    //hardware reset
    gpiod_set_value(ctx->reset, 1);
    msleep(20);
    gpiod_set_value(ctx->reset, 0);  
    msleep(20);

    for (i = 0; i < ctx->desc->init_length; i++){
        const struct ek79007_instr* instr = &ctx->desc->init[i];

        if (instr->op == EK79007_SWITCH_PAGE)
                ret = ek79007_switch_page(ctx, instr->arg.page);
        else if(instr->op == EK79007_COMMAND)
                ret = ek79007_send_cmd_data(ctx, instr->arg.cmd.cmd, instr->arg.cmd.data);

        if (ret)
            return ret;
    }

    ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);

    if (ret)
        return ret;

    msleep(120);

    ret = mipi_dsi_dcs_set_display_on(ctx->dsi);

    if (ret)
        return ret;

    msleep(120);

    return 0;
}

static int ek79007_enable(struct drm_panel *panel){
    struct ek79007 *ctx = panel_to_ek79007(panel);

    msleep(120);

    return mipi_dsi_dcs_set_display_on(ctx->dsi);
}

static int ek79007_disable(struct drm_panel *panel){
    struct ek79007 *ctx = panel_to_ek79007(panel);
    
    return mipi_dsi_dcs_set_display_off(ctx->dsi);
}

static int ek79007_unprepare(struct drm_panel *panel){
    struct ek79007 *ctx = panel_to_ek79007(panel);

    mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
    regulator_disable(ctx->power);
    gpiod_set_value(ctx->reset, 1);

    return 0; 
}

static const struct drm_display_mode LX700B4008CTP14_default_mode = {
    .clock          = 51260,
    
    .hdisplay       = 1024,
    .hsync_start    = 1024 + 160,
    .hsync_end      = 1024 + 160 + 1,
    .htotal         = 1024 + 160 + 160 + 1,

    .vdisplay       = 600,
    .vsync_start    = 600 + 10,
    .vsync_end      = 600 + 10 + 2,
    .htotal         = 600 + 10+ 10 + 2,

    .width_mm       = 154,
    .height_mm	    = 85,
}

static int ek79007_get_modes(struct drm_panel *panel, struct drm_connector *connector){
    struct ek79007 *ctx = panel_to_ek79007(panel);
    struct drm_display_mode *mode;

    mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
    if(!mode){
        dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
                ctx->desc->mode->hdisplay,
                ctx->desc->mode->vdisplay,
                drm_mode_vrefresh(ctx->desc->mode));
        
        return -ENOMEM;
    }

    drm_mode_set_name(mode);

    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    drm_mode_probe_add(connector, mode);

    connector->display_info.width_mm    = mode->width_mm;
    connector->display_info.height_mm   = mode->height_mm;

    return 1;
}

static const struct drm_panel_funcs ek79007_funcs = {
    .prepare    = ek79007_prepare,
    .unprepare  = ek79007_unprepare,
    .enable     = ek79007_enable,
    .disable    = ek79007_disable,
    .get_modes  = ek79007_get_modes,
};

static int ek79007_dsi_probe(struct mipi_dsi_device *dsi){
    struct ek79007 *ctx;
    int ret;

    pr_info("EK79007 probe\n");

    ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
    if(!ctx)
        return -ENOMEM;

    mipi_dsi_set_drvdata(dsi, ctx);
    ctx->dsi    = dsi;
    ctx->desc   = of_device_get_match_data(&dsi->dev);

    drm_panel_init(&ctx->panel, &dsi->dev, &ek79007_funcs,
                    DRM_MODE_CONNECTOR_DSI);
    
    ctx->power = devm_regulator_get(&dsi->dev, "power");
    if(IS_ERR(ctx->power)){
        dev_err(&dsi->dev, "Couldn't get our power regulator\n");
        return PTR_ERR(ctx->power);
    }

    ctx->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
    if(IS_ERR(ctx->reset)){
        dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
        return PTR_ERR(ctx->reset);
    }

    ret = drm_panel_of_backlight(&ctx->panel);

    if(ret)
        return ret;

    backlight_disable(ctx->panel.backlight);

    drm_panel_add(&ctx->panel);
    dsi->mode_flags = MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
    dsi->format     = MIPI_DSI_FMT_RGB888;
    dsi->lanes      = 2;

    return mipi_dsi_attach(dsi);
}

static int ek79007_dsi_remove(struct mipi_dsi_device *dsi){
    struct ek79007 *ctx = mipi_dsi_get_drvdata(dsi);

    pr_info("EK79007 remove\n");

    mipi_dsi_detach(dsi);
    drm_panel_remove(&ctx->panel);

    return 0;
}

static const struct ek79007_desc LX700B4008CTP14_desc = {
    .init           = LX700B4008CTP14_init,
    .init_length    = ARRAY_SIZE(LX700B4008CTP14_init),
    .mode           = &LX700B4008CTP14_default_mode,
};

static const struct of_device_id ek79007_of_match[] = {
    { .compatible = "fitipower, LX700B4008CTP14", .data = &LX700B4008CTP14_desc},
    { }
};

MODULE_DEVICE_TABLE(of, ek79007_of_match);

static struct mipi_dsi_driver ek79007_dsi_driver = {
        .probe      = ek79007_dsi_probe,
        .remove     = ek79007_dsi_remove,
        .driver     = {
                    .name           = "ek79007-dsi",
                    .of_match_table = ek79007_of_match,
        };
};

module_mipi_dsi_driver(ek79007_dsi_driver);

MODULE_AUTHOR("MRosner");
MODULE_DESCRIPTION("EK79007 mipi dsi driver");
MODULE_LICENCE("GPL v2");