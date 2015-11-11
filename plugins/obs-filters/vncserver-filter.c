#include <obs-module.h>
#include <graphics/vec4.h>
#include <pthread.h>
#include <unistd.h>
#include <rfb/rfb.h>

#define SETTING_DISPLAY                "Display"

#define TEXT_DISPLAY                   obs_module_text("Display Number")

//#define MIN_DISPLAY 0
//#define MAX_DISPLAY 99

struct vncpreview_filter_data {
	obs_source_t                   *context;

//	gs_effect_t                    *effect;

	gs_eparam_t                    *display;

    pthread_t                       thread;
    pthread_mutex_t                 fb_mutex;

    int                             update;
    int                             _frame;
    char                           *fb;
    int                             destroy;
};

const char *vncpreview_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Preview VNC Service");
}

static void vncpreview_filter_update(void *data, obs_data_t *settings)
{
    return;

	/* 
    struct vncpreview_filter_data *filter = data;
	uint32_t display = (uint32_t)obs_data_get_int(settings,
		SETTING_DISPLAY);
    */
}

static void vncpreview_filter_destroy(void *data)
{
	struct vncpreview_filter_data *filter = data;

    if (filter->thread) {
        filter->destroy = 1;
        pthread_join(filter->thread, NULL);
    }

//	if (filter->effect) {
//		obs_enter_graphics();
//		gs_effect_destroy(filter->effect);
//		obs_leave_graphics();
//	}

	bfree(data);
}

int vncserver_grab_frame(struct vncpreview_filter_data *filter)
{
    if (filter->fb == NULL)
        return 0;

    if (! filter->update)
    {
        // printf("frame is not updated. waiting for next frame\n");
        while (! filter->update)
            usleep(1000);
    }

    /* Update VNC frame buffer */
    pthread_mutex_lock(&filter->fb_mutex);
    memset(filter->fb, filter->_frame & 0xFF, (1280 * 720 * 4));
    filter->_frame += 16;
    filter->update = 0;
    pthread_mutex_unlock(&filter->fb_mutex);

    return 1;
}

void *vncserver_worker_thread(void *param)
{
    struct vncpreview_filter_data *filter = (struct vncpreview_filter_data *) param;
    int WIDTH = 1280;
    int HEIGHT = 720;
    int BPP = 4;

    printf("vncserver worker thread started\n");
    int argc = 0;
    rfbScreenInfoPtr server=rfbGetScreen(&argc, NULL ,WIDTH,HEIGHT,8,3,BPP);
    if(!server)
        return 0;

    server->desktopName = "OBS Preview VNC Service";
    server->frameBuffer=(char*)malloc(WIDTH*HEIGHT*BPP);
    server->alwaysShared=(1==1);

    filter->fb = server->frameBuffer;

    /* Initialize the server */
    rfbInitServer(server);

    /* Loop, processing clients and taking pictures */
    while ((!filter->destroy) && rfbIsActive(server)) {
        if (1) //TimeToTakePicture())
            //if (TakePicture((unsigned char *)server->frameBuffer))
                vncserver_grab_frame(filter);
                rfbMarkRectAsModified(server,0,0,WIDTH,HEIGHT);

        //usec = server->deferUpdateTime*1000;
        rfbProcessEvents(server,1000);
    }

    printf("vncserver worker thread stopped\n");
}

static void *vncpreview_filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct vncpreview_filter_data *filter =
		bzalloc(sizeof(struct vncpreview_filter_data));
//	char *effect_path = obs_module_file("preview_vncserver.service");

    pthread_mutex_init(&filter->fb_mutex, NULL);
    pthread_create(&filter->thread, NULL, &vncserver_worker_thread, filter);

	filter->context = context;

	obs_enter_graphics();

//	filter->effect = gs_effect_create_from_file(effect_path, NULL);
//	if (filter->effect) {
//		filter->color_param = gs_effect_get_param_by_name(
//				filter->effect, "color");
//	}
	obs_leave_graphics();

//	bfree(effect_path);

//	if (!filter->effect) {
//		vncpreview_filter_destroy(filter);
//		return NULL;
//	}

	vncpreview_filter_update(filter, settings);
	return filter;
}

static void vncpreview_filter_render(void *data, gs_effect_t *effect)
{
	struct vncpreview_filter_data *filter = data;
    int height, width;

    if (pthread_mutex_trylock(&filter->fb_mutex) == 0)
    {
        /* FIXME: Lock OBS */
        /*
        obs_source_process_filter_begin(filter->context, GS_RGBA,
        		OBS_ALLOW_DIRECT_RENDERING);

        obs_source_process_filter_end(filter->context, filter->effect, 0, 0);
        */

        height = obs_source_get_height(filter->context);
        width = obs_source_get_width(filter->context);

        printf("height %d width %d\n", height, width);

        obs_source_skip_video_filter(filter->context);
        filter->update = 1;
        pthread_mutex_unlock(&filter->fb_mutex);
    } else {
        printf("vncpreview_filter_render: dropped a frame (mutex locked)\n");
    }

	UNUSED_PARAMETER(effect);
}

obs_properties_t *vncpreview_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, SETTING_DISPLAY, TEXT_DISPLAY,
			0, 100, 1);

	UNUSED_PARAMETER(data);
	return props;
}

static void vncpreview_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_DISPLAY, 0);
}

struct obs_source_info vncpreview_filter = {
	.id                            = "vncpreview_filter",
	.type                          = OBS_SOURCE_TYPE_FILTER,
	.output_flags                  = OBS_SOURCE_VIDEO,
	.get_name                      = vncpreview_filter_name,
	.create                        = vncpreview_filter_create,
	.destroy                       = vncpreview_filter_destroy,
	.video_render                  = vncpreview_filter_render,
	.update                        = vncpreview_filter_update,
	.get_properties                = vncpreview_filter_properties,
	.get_defaults                  = vncpreview_filter_defaults
};
