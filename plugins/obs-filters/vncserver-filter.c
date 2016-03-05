#include "obs.h"
#include "obs-internal.h"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <pthread.h>
#include <unistd.h>
#include <rfb/rfb.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/mman.h>


#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>


#define SETTING_DISPLAY                "Display"

#define TEXT_DISPLAY                   obs_module_text("Display Number")

//#define MIN_DISPLAY 0
//#define MAX_DISPLAY 99

struct vncpreview_filter_data {
	obs_source_t                   *context;

    int                             update;
    pthread_mutex_t                 fb_mutex;
//	gs_effect_t                    *effect;

	gs_eparam_t                    *display;

    pthread_t                       thread;

    char                           *fb;
    int                             fb_linesize;
	AVPicture          dst_picture;
    int                             destroy;

	struct SwsContext  *swscale;
    char *shm_fb;
    int shm_fd;
};

const char *vncpreview_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Preview VNC Service");
}

static void vncpreview_render_display(void *data, uint32_t cx, uint32_t cy)
{
    blog(LOG_ERROR, "CALLBACK!");

	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
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
        // pthread_join(filter->thread, NULL);
    }

//	if (filter->effect) {
//		obs_enter_graphics();
//		gs_effect_destroy(filter->effect);
//		obs_leave_graphics();
//	}

	bfree(data);
}


#define RNDTO2(X) ( ( (X) & 0xFFFFFFFE ) )
#define RNDTO32(X) ( ( (X) % 32 ) ? ( ( (X) + 32 ) & 0xFFFFFFE0 ) : (X) )

static void vncserver_render_callback(void *param, struct video_data *frame)
{
    struct vncpreview_filter_data *filter = param;
    static struct SwsContext *swscale;

    filter->swscale = NULL;
//    enum video_format format = video_output_get_format(video);

#if 0
    if (pthread_mutex_trylock(&filter->fb_mutex) != 0)
        // Failed to lock.
        // Probably another process is accessing to the buffer.
        return;

    if (0) //filter->update == 0)
    {
        pthread_mutex_unlock(&filter->fb_mutex);
        return;
    }
#else
    pthread_mutex_unlock(&filter->fb_mutex);
#endif

    struct video_t *video = obs_get_video();
	const struct video_output_info *voi = video_output_get_info(video);
    int width    = voi->width;
    int height   = voi->height;
#if 0
    swsCtx = sws_getCachedContext ( swsCtx, width, height,
        AV_PIX_FMT_NV12, width, height, AV_PIX_FMT_RGBA, 
            SWS_LANCZOS | SWS_ACCURATE_RND , NULL, NULL, NULL );
#else
    if (filter->swscale) {
        swscale = filter->swscale;
    } else {
        swscale = sws_getContext(
            width, height, AV_PIX_FMT_NV12,
            width, height, AV_PIX_FMT_RGBA,
            0, NULL, NULL, NULL
        );
        filter->swscale = swscale;
    }
#endif
    sws_scale(
        swscale,
        (const uint8_t *const *)frame->data,
        (const int*)frame->linesize,
        0, height, &filter->fb,
        filter->dst_picture.linesize
    );

    if ( filter->shm_fb) {
#if 1

        int y;
        for (y = 0; y < 720; y++) {
            char *src = (char*)filter->fb + (y * filter->dst_picture.linesize[0]);
            char *dest = (char*) filter->shm_fb + y * (1280 * 4);
            memcpy(dest, src, 1280*4);
        }
#endif
        // memcpy(filter->shm_fb, frame->data, (1280*720*4));

    }

    filter->update = 1;

    pthread_mutex_unlock(&filter->fb_mutex);
}

static void vncserver_render()
{
}

static void vncserver_init_video(struct vncpreview_filter_data *filter)
{
    struct video_t *video = obs_get_video();
    enum video_format format = video_output_get_format(video);
    printf("obs_get_video() = %p\n", video);
    printf("format = %d\n", format);

	const struct video_output_info *voi = video_output_get_info(video);
    printf("voi: format %d fps %d w %d h %d\n",
        voi->format,
        voi->fps_num,
        voi->width,
        voi->height
    );

    bool r = video_output_connect(video, NULL, vncserver_render_callback, filter);
    if (r) {
        printf("vncserver-filter: Success to connect\n");
    } else {
        printf("vncserver-filter: Failed to connect\n");
    }

    const char *IKALOG_SHM_NAME="IKALOG_FB";
    const int IKALOG_SHM_SIZE=1280*720*4;

    filter->shm_fd = shm_open(IKALOG_SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(filter->shm_fd, IKALOG_SHM_SIZE);

    filter->shm_fb = mmap(0, IKALOG_SHM_SIZE, PROT_READ |PROT_WRITE, MAP_SHARED, filter->shm_fd, 0);
    printf("vncserver-filiter: shared memory at %p, size %d\n", filter->shm_fb, IKALOG_SHM_SIZE);

}

static int vncserver_grab_frame(rfbScreenInfoPtr server, struct vncpreview_filter_data *filter)
{
    if (filter->fb == NULL)
        return 0;

//    if (! filter->update)
//        return 0;

    /* Update VNC frame buffer */
    //pthread_mutex_lock(&filter->fb_mutex);
    memcpy(server->frameBuffer, filter->fb, 1280 * 720 * 4);
    filter->update = 0;
    //pthread_mutex_unlock(&filter->fb_mutex);

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

    filter->fb = (char*)malloc(WIDTH * HEIGHT * BPP);
    filter->fb_linesize = 1280 * 4;
    int ret = avpicture_alloc(&filter->dst_picture, AV_PIX_FMT_BGRA, 1280, 720);
    filter->dst_picture.linesize[0] = 1280 * 4;

    return 0;

    server->alwaysShared=(1==1);


    /* Initialize the server */
    rfbInitServer(server);

    /* Loop, processing clients and taking pictures */
    while ((!filter->destroy) && rfbIsActive(server)) {
        int updated= 0;

        if (1)
        updated = vncserver_grab_frame(server, filter);

        if(1)
            rfbMarkRectAsModified(server,0,0,WIDTH,HEIGHT);

        //usec = server->deferUpdateTime*1000;
        rfbProcessEvents(server,100000);
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
    vncserver_init_video(filter);
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

        // printf("height %d width %d\n", height, width);

        obs_source_skip_video_filter(filter->context);
        filter->update = 1;
        pthread_mutex_unlock(&filter->fb_mutex);
    } else {
        // printf("vncpreview_filter_render: dropped a frame (mutex locked)\n");
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
