#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/param.h>
#include <inttypes.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <linux/socket.h>
#include <fcntl.h>
#include <pthread.h>

#include "cvi_buffer.h"
#include "sample_comm.h"
#include "cvi_sys.h"

#define MAX_VDEC_NUM 4
#define AIKB_PANEL_WIDTH 412
#define AIKB_PANEL_HEIGHT 960
#define AIKB_VIDEO_WIDTH AIKB_PANEL_WIDTH
#define AIKB_VIDEO_HEIGHT AIKB_PANEL_HEIGHT
#define AIKB_VIDEO_CTRL_PATH "/tmp/aikb_video_ctrl"
#define AIKB_VIDEO_PATH_MAX 255

typedef struct _SAMPLE_VDEC_PARAM_S {
	VDEC_CHN        VdecChn;
	VDEC_CHN_ATTR_S stChnAttr;
	char            decode_file_name[AIKB_VIDEO_PATH_MAX + 1];
	CVI_BOOL        stop_thread;
	pthread_t       vdec_thread;
	RECT_S          stDispRect;
} SAMPLE_VDEC_PARAM_S;

typedef struct _SAMPLE_VDEC_CONFIG_S {
	CVI_S32 s32ChnNum;
	SAMPLE_VDEC_PARAM_S astVdecParam[MAX_VDEC_NUM];
} SAMPLE_VDEC_CONFIG_S;

static pthread_t send_vo_thread;
static CVI_VOID *s_h264file[MAX_VDEC_NUM];
static bool is_using_vo = true;
static bool is_running = true;
static pthread_mutex_t g_video_ctrl_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_current_video_file[AIKB_VIDEO_PATH_MAX + 1];
static char g_pending_video_file[AIKB_VIDEO_PATH_MAX + 1];
static pthread_t g_video_ctrl_thread;
static bool g_video_ctrl_thread_started;

static void strip_line_end(char *line)
{
	size_t len = strlen(line);

	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
		line[len - 1] = '\0';
		len--;
	}
}

static void note_current_video_file(const char *path)
{
	pthread_mutex_lock(&g_video_ctrl_lock);
	snprintf(g_current_video_file, sizeof(g_current_video_file), "%s", path);
	pthread_mutex_unlock(&g_video_ctrl_lock);
}

static void request_video_file_switch(const char *path)
{
	pthread_mutex_lock(&g_video_ctrl_lock);
	if (strcmp(path, g_current_video_file) != 0)
		snprintf(g_pending_video_file, sizeof(g_pending_video_file), "%s", path);
	pthread_mutex_unlock(&g_video_ctrl_lock);
}

static bool take_pending_video_file(char *path, size_t path_size)
{
	bool has_pending = false;

	pthread_mutex_lock(&g_video_ctrl_lock);
	if (g_pending_video_file[0] != '\0') {
		snprintf(path, path_size, "%s", g_pending_video_file);
		g_pending_video_file[0] = '\0';
		has_pending = true;
	}
	pthread_mutex_unlock(&g_video_ctrl_lock);
	return has_pending;
}

static CVI_VOID *thread_video_ctrl(CVI_VOID *arg)
{
	(void)arg;
	prctl(PR_SET_NAME, "aikb-video-ctrl");

	while (is_running) {
		char line[AIKB_VIDEO_PATH_MAX + 16];
		int fd = open(AIKB_VIDEO_CTRL_PATH, O_RDWR);
		FILE *fp;

		if (fd < 0) {
			perror("open video ctrl");
			sleep(1);
			continue;
		}
		fp = fdopen(fd, "r");
		if (fp == NULL) {
			perror("fdopen video ctrl");
			close(fd);
			sleep(1);
			continue;
		}
		while (is_running && fgets(line, sizeof(line), fp) != NULL) {
			char *path = line;

			strip_line_end(line);
			if (strncmp(line, "file ", 5) == 0)
				path = line + 5;
			if (path[0] == '\0')
				continue;
			if (access(path, R_OK) != 0) {
				printf("video ctrl missing file: %s\n", path);
				continue;
			}
			printf("video ctrl switch: %s\n", path);
			request_video_file_switch(path);
		}
		fclose(fp);
	}
	return NULL;
}

static void start_video_ctrl_thread(void)
{
	struct stat st;

	if (stat(AIKB_VIDEO_CTRL_PATH, &st) == 0) {
		if (!S_ISFIFO(st.st_mode)) {
			unlink(AIKB_VIDEO_CTRL_PATH);
			(void)mkfifo(AIKB_VIDEO_CTRL_PATH, 0600);
		}
	} else {
		(void)mkfifo(AIKB_VIDEO_CTRL_PATH, 0600);
	}
	if (pthread_create(&g_video_ctrl_thread, NULL, thread_video_ctrl, NULL) == 0)
		g_video_ctrl_thread_started = true;
	else
		perror("pthread_create video ctrl");
}

static void set_single_video_display_rect(SAMPLE_VDEC_PARAM_S *param)
{
	/*
	 * Keep the decoded frame in the panel's physical 412x960 orientation.
	 * The source video is pre-rotated during conversion so VO can receive
	 * frames that match the enabled channel size.
	 */
	const CVI_U32 canvas_w = AIKB_VIDEO_WIDTH;
	const CVI_U32 canvas_h = AIKB_VIDEO_HEIGHT;
	const CVI_U32 src_w = AIKB_VIDEO_WIDTH;
	const CVI_U32 src_h = AIKB_VIDEO_HEIGHT;
	CVI_U32 out_w = canvas_w;
	CVI_U32 out_h = (canvas_w * src_h + src_w / 2) / src_w;

	if (out_h > canvas_h) {
		out_h = canvas_h;
		out_w = (canvas_h * src_w + src_h / 2) / src_h;
	}
	param->stDispRect.s32X = (canvas_w - out_w) / 2;
	param->stDispRect.s32Y = (canvas_h - out_h) / 2;
	param->stDispRect.u32Width = out_w;
	param->stDispRect.u32Height = out_h;
}

#define SHOW_STATISTICS_1
// #define SHOW_STATISTICS_2

CVI_VOID *thread_vdec_send_stream(CVI_VOID *arg)
{
	FILE *fpStrm = NULL;
	SAMPLE_VDEC_PARAM_S *param = (SAMPLE_VDEC_PARAM_S *)arg;
	CVI_BOOL bEndOfStream = CVI_FALSE;
	CVI_S32 s32UsedBytes = 0, s32ReadLen = 0;
	CVI_U8 *pu8Buf = NULL;
	VDEC_STREAM_S stStream;
	CVI_BOOL bFindStart, bFindEnd;
	CVI_U64 u64PTS = 0;
	CVI_U32 u32Len, u32Start;
	CVI_S32 s32Ret, i;
	CVI_S32 bufsize = (param->stChnAttr.u32PicWidth * param->stChnAttr.u32PicHeight * 3) >> 1;
	char strBuf[64];
	int cur_cnt = 0;
#ifdef SHOW_STATISTICS_1
	struct timeval pre_tv, tv1, tv2;
	int pre_cnt = 0;
	int accum_ms = 0;

	pre_tv.tv_usec =
	pre_tv.tv_sec = 0;
#endif

	snprintf(strBuf, sizeof(strBuf), "thread_vdec-%d", param->VdecChn);
	prctl(PR_SET_NAME, strBuf);

	fpStrm = fopen(param->decode_file_name, "rb");
	if (fpStrm == NULL) {
		printf("open file err, %s\n", param->decode_file_name);
		return (CVI_VOID *)(CVI_FAILURE);
	}
	pu8Buf = malloc(bufsize);
	if (pu8Buf == NULL) {
		printf("chn %d can't alloc %d in send stream thread!\n",
				param->VdecChn,
				bufsize);
		fclose(fpStrm);
		return (CVI_VOID *)(CVI_FAILURE);
	}
	printf("thread_vdec_send_stream %d\n", param->VdecChn);
	note_current_video_file(param->decode_file_name);
	u64PTS = 0;
	while (!param->stop_thread) {
		int retry = 0;
		char next_file[AIKB_VIDEO_PATH_MAX + 1];

		if (param->VdecChn == 0 &&
		    take_pending_video_file(next_file, sizeof(next_file))) {
			FILE *next_fp = fopen(next_file, "rb");

			if (next_fp == NULL) {
				printf("open switch file err, %s\n", next_file);
			} else {
				fclose(fpStrm);
				fpStrm = next_fp;
				snprintf(param->decode_file_name,
					 sizeof(param->decode_file_name),
					 "%s", next_file);
				s32UsedBytes = 0;
				u64PTS = 0;
				note_current_video_file(next_file);
				printf("video switched to %s\n", next_file);
			}
		}
		bEndOfStream = CVI_FALSE;
		bFindStart = CVI_FALSE;
		bFindEnd = CVI_FALSE;
		u32Start = 0;
		s32Ret = fseek(fpStrm, s32UsedBytes, SEEK_SET);
		s32ReadLen = fread(pu8Buf, 1, bufsize, fpStrm);
		if (s32ReadLen == 0) {//file end
			s32UsedBytes = 0;
			fseek(fpStrm, 0, SEEK_SET);
			s32ReadLen = fread(pu8Buf, 1, bufsize, fpStrm);

		}
		if (param->stChnAttr.enMode == VIDEO_MODE_FRAME &&
				param->stChnAttr.enType == PT_H264) {
			for (i = 0; i < s32ReadLen - 8; i++) {
				int tmp = pu8Buf[i + 3] & 0x1F;

				if (pu8Buf[i] == 0 && pu8Buf[i + 1] == 0 && pu8Buf[i + 2] == 1 &&
				    (((tmp == 0x5 || tmp == 0x1) && ((pu8Buf[i + 4] & 0x80) == 0x80)) ||
				     (tmp == 20 && (pu8Buf[i + 7] & 0x80) == 0x80))) {
					bFindStart = CVI_TRUE;
					i += 8;
					break;
				}
			}

			for (; i < s32ReadLen - 8; i++) {
				int tmp = pu8Buf[i + 3] & 0x1F;

				if (pu8Buf[i] == 0 && pu8Buf[i + 1] == 0 && pu8Buf[i + 2] == 1 &&
				    (tmp == 15 || tmp == 7 || tmp == 8 || tmp == 6 ||
				     ((tmp == 5 || tmp == 1) && ((pu8Buf[i + 4] & 0x80) == 0x80)) ||
				     (tmp == 20 && (pu8Buf[i + 7] & 0x80) == 0x80))) {
					bFindEnd = CVI_TRUE;
					break;
				}
			}

			if (i > 0)
				s32ReadLen = i;
			if (bFindStart == CVI_FALSE) {
				CVI_VDEC_TRACE("chn 0 can not find H264 start code!s32ReadLen %d, s32UsedBytes %d.!\n",
					    s32ReadLen, s32UsedBytes);
			}
			if (bFindEnd == CVI_FALSE) {
				s32ReadLen = i + 8;
			}

		} else if (param->stChnAttr.enMode == VIDEO_MODE_FRAME &&
				param->stChnAttr.enType == PT_H265) {
			CVI_BOOL bNewPic = CVI_FALSE;

			for (i = 0; i < s32ReadLen - 6; i++) {
				CVI_U32 tmp = (pu8Buf[i + 3] & 0x7E) >> 1;

				bNewPic = (pu8Buf[i + 0] == 0 && pu8Buf[i + 1] == 0 && pu8Buf[i + 2] == 1 &&
					   (tmp <= 21) && ((pu8Buf[i + 5] & 0x80) == 0x80));

				if (bNewPic) {
					bFindStart = CVI_TRUE;
					i += 6;
					break;
				}
			}

			for (; i < s32ReadLen - 6; i++) {
				CVI_U32 tmp = (pu8Buf[i + 3] & 0x7E) >> 1;

				bNewPic = (pu8Buf[i + 0] == 0 && pu8Buf[i + 1] == 0 && pu8Buf[i + 2] == 1 &&
					   (tmp == 32 || tmp == 33 || tmp == 34 || tmp == 39 || tmp == 40 ||
					    ((tmp <= 21) && (pu8Buf[i + 5] & 0x80) == 0x80)));

				if (bNewPic) {
					bFindEnd = CVI_TRUE;
					break;
				}
			}
			if (i > 0)
				s32ReadLen = i;

			if (bFindStart == CVI_FALSE) {
				CVI_VDEC_TRACE("chn 0 can not find H265 start code!s32ReadLen %d, s32UsedBytes %d.!\n",
					    s32ReadLen, s32UsedBytes);
			}
			if (bFindEnd == CVI_FALSE) {
				s32ReadLen = i + 6;
			}

		} else if (param->stChnAttr.enType == PT_MJPEG
				|| param->stChnAttr.enType == PT_JPEG) {
			for (i = 0; i < s32ReadLen - 1; i++) {
				if (pu8Buf[i] == 0xFF && pu8Buf[i + 1] == 0xD8) {
					u32Start = i;
					bFindStart = CVI_TRUE;
					i = i + 2;
					break;
				}
			}

			for (; i < s32ReadLen - 3; i++) {
				if ((pu8Buf[i] == 0xFF) && (pu8Buf[i + 1] & 0xF0) == 0xE0) {
					u32Len = (pu8Buf[i + 2] << 8) + pu8Buf[i + 3];
					i += 1 + u32Len;
				} else {
					break;
				}
			}

			for (; i < s32ReadLen - 1; i++) {
				if (pu8Buf[i] == 0xFF && pu8Buf[i + 1] == 0xD9) {
					bFindEnd = CVI_TRUE;
					break;
				}
			}
			s32ReadLen = i + 2;

			if (bFindStart == CVI_FALSE) {
				CVI_VDEC_TRACE("chn 0 can not find JPEG start code!s32ReadLen %d, s32UsedBytes %d.!\n",
					    s32ReadLen, s32UsedBytes);
			}
		} else {
			if ((s32ReadLen != 0) && (s32ReadLen < bufsize)) {
				bEndOfStream = CVI_TRUE;
			}
		}

		stStream.u64PTS = u64PTS;
		stStream.pu8Addr = pu8Buf + u32Start;
		stStream.u32Len = s32ReadLen;
		stStream.bEndOfFrame = CVI_TRUE;
		stStream.bEndOfStream = bEndOfStream;
		stStream.bDisplay = 1;
SendAgain:
#ifdef SHOW_STATISTICS_1
		gettimeofday(&tv1, NULL);
#endif
		CVI_SYS_TraceBegin("CVI_VDEC_SendStream");
		s32Ret = CVI_VDEC_SendStream(param->VdecChn,
					&stStream, -1);
		CVI_SYS_TraceEnd();
		if (s32Ret != CVI_SUCCESS) {
			//printf("%d dec chn CVI_VDEC_SendStream err ret=%d\n",param->vdec_chn,s32Ret);
			retry++;
			if (param->stop_thread)
				break;
			usleep(10000);
			goto SendAgain;
		} else {
			bEndOfStream = CVI_FALSE;
			s32UsedBytes = s32UsedBytes + s32ReadLen + u32Start;
			u64PTS += 1;
			cur_cnt++;
		}
#ifdef SHOW_STATISTICS_1
		gettimeofday(&tv2, NULL);
		int curr_ms =
			(tv2.tv_sec - tv1.tv_sec) * 1000 + (tv2.tv_usec/1000) - (tv1.tv_usec/1000);

		accum_ms += curr_ms;
		if (pre_tv.tv_usec == 0 && pre_tv.tv_sec == 0) {
			pre_tv = tv2;
		} else {
			unsigned long diffus = 0;

			if (pre_tv.tv_sec == tv2.tv_sec) {
				if (tv2.tv_usec > pre_tv.tv_usec) {
					diffus = tv2.tv_usec - pre_tv.tv_usec;
				}
			} else if (tv2.tv_sec > pre_tv.tv_sec) {
				diffus = (tv2.tv_sec - pre_tv.tv_sec) * 1000000;
				diffus = diffus + tv2.tv_usec - pre_tv.tv_usec;
			}

			if (diffus == 0) {
				pre_tv = tv2;
				pre_cnt = cur_cnt;
			} else if (diffus > 1000000) {
				int add_cnt = cur_cnt - pre_cnt;
				double avg_fps = (add_cnt * 1000000.0 / (double) diffus);

				printf("[%d] CVI_VDEC_SendStream Avg. %d ms FPS %.2lf\n"
						, param->VdecChn, accum_ms / add_cnt, avg_fps);
				pre_tv = tv2;
				pre_cnt = cur_cnt;
				accum_ms = 0;
			}
		}
#endif
		usleep(10000);

	}
	printf("thread_vdec_send_stream %d exit\n", param->VdecChn);
	fclose(fpStrm);
	free(pu8Buf);
	return NULL;
}

CVI_S32 set_vpss_AspectRatio(int index, VPSS_GRP VpssGrp, VPSS_CHN VpssChn, RECT_S *rect)
{
	CVI_S32 s32Ret = CVI_SUCCESS;
	VPSS_CHN_ATTR_S stChnAttr;

	s32Ret = CVI_VPSS_GetChnAttr(VpssGrp, VpssChn, &stChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_VPSS_GetChnAttr is fail\n");
		return s32Ret;
	}
	stChnAttr.stAspectRatio.stVideoRect.s32X = rect->s32X;
	stChnAttr.stAspectRatio.stVideoRect.s32Y = rect->s32Y;
	stChnAttr.stAspectRatio.stVideoRect.u32Width = rect->u32Width;
	stChnAttr.stAspectRatio.stVideoRect.u32Height = rect->u32Height;
	stChnAttr.stAspectRatio.enMode        = ASPECT_RATIO_MANUAL;
	if (index == 0) {
		stChnAttr.stAspectRatio.bEnableBgColor = CVI_TRUE;
		stChnAttr.stAspectRatio.u32BgColor = RGB_8BIT(0, 0, 0);
	} else {
		stChnAttr.stAspectRatio.bEnableBgColor = CVI_FALSE;
	}

	s32Ret = CVI_VPSS_SetChnAttr(VpssGrp, VpssChn, &stChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_SetChnAttr grp %d failed with %#x\n", VpssGrp, s32Ret);
	}

	return s32Ret;
}

int alarm_sockets[2];
CVI_VOID *thread_send_vo(CVI_VOID *arg)
{
	CVI_S32 s32Ret = CVI_SUCCESS;
	CVI_BOOL bFirstFrame = CVI_FALSE;
	SAMPLE_VDEC_CONFIG_S *pstVdecCfg = (SAMPLE_VDEC_CONFIG_S *)arg;
	SAMPLE_VDEC_PARAM_S *pstVdecChn;
	VIDEO_FRAME_INFO_S stVdecFrame = {0};
	VIDEO_FRAME_INFO_S stOverlayFrame = {0};
	VPSS_GRP VpssGrp1 = 1;
	VPSS_CHN VpssChn = 0;
	VO_LAYER VoLayer = 0;
	VO_CHN VoChn = 0;
	int retry = 0;
	int frm_cnt = 0;
#ifdef SHOW_STATISTICS_2
	struct timeval tv1, tv2;
#endif
	fd_set readfds;
	struct timeval tv;

	prctl(PR_SET_NAME, "thread_send_vo");
#ifdef SHOW_STATISTICS_2
	tv1.tv_usec = tv1.tv_sec = 0;
#endif
	while (!pstVdecCfg->astVdecParam[0].stop_thread) {
		int fail = 0;
		int r;

		FD_ZERO(&readfds);
		FD_SET(alarm_sockets[1], &readfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = select(alarm_sockets[1] + 1, &readfds, NULL, NULL, &tv);
		if (r == -1) {
			if (errno == EINTR) {
				fprintf(stderr, "%s: select interrupt\n", __func__);
				continue;
			}
			continue;
		} else if (r == 0) {
			struct itimerval cur;

			getitimer(ITIMER_REAL, &cur);
			fprintf(stderr, "%s: select timeout. %ld %d\n", __func__
				, cur.it_value.tv_usec, frm_cnt);
			// continue;
		} else {
			char buf[240];
			int ret = read(alarm_sockets[1], buf, sizeof(buf));

			if (ret != 4)
				fprintf(stderr, "select read %d\n", ret);
		}

		bFirstFrame = CVI_TRUE;

		//vdec frame
		for (CVI_S32 i = 0; i < pstVdecCfg->s32ChnNum; i++) {
			pstVdecChn = &pstVdecCfg->astVdecParam[i];

			if (!bFirstFrame) {
				s32Ret = CVI_VPSS_SendChnFrame(VpssGrp1, VpssChn, &stOverlayFrame, 1000);
				if (s32Ret != CVI_SUCCESS) {
					CVI_VPSS_ReleaseChnFrame(VpssGrp1, VpssChn, &stOverlayFrame);
					continue;
				}
				CVI_VPSS_ReleaseChnFrame(VpssGrp1, VpssChn, &stOverlayFrame);
			}
			CVI_SYS_TraceBegin("CVI_VDEC_GetFrame");
RETRY_GET_FRAME:
			s32Ret = CVI_VDEC_GetFrame(pstVdecChn->VdecChn, &stVdecFrame, 1000);
			CVI_SYS_TraceEnd();
			if (s32Ret != CVI_SUCCESS) {
				// printf("[%d] CVI_VDEC_GetFrame fail\n", pstVdecChn->VdecChn);
				fail++;
				retry++;
				if (s32Ret == CVI_ERR_VDEC_BUSY) {
					printf("get frame timeout ..in overlay ..retry\n");
					goto RETRY_GET_FRAME;
				}
				break;
				// continue;
			}

			set_vpss_AspectRatio(i, VpssGrp1, VpssChn, &pstVdecChn->stDispRect);
			s32Ret = CVI_VPSS_SendFrame(VpssGrp1, &stVdecFrame, 1000);

			if (s32Ret != CVI_SUCCESS) {
				CVI_VDEC_ReleaseFrame(pstVdecChn->VdecChn, &stVdecFrame);
				continue;
			}

			s32Ret = CVI_VPSS_GetChnFrame(VpssGrp1, VpssChn, &stOverlayFrame, 1000);
			CVI_VDEC_ReleaseFrame(pstVdecChn->VdecChn, &stVdecFrame);
			if (s32Ret != CVI_SUCCESS) {
				printf("CVI_VPSS_GetChnFrame faile, grp:%d\n", VpssGrp1);
				continue;
			}
			bFirstFrame = CVI_FALSE;
		}
		if (fail > 0) {
			// usleep(10000);
			continue;
		}

		if (is_using_vo) {
			s32Ret = CVI_VO_SendFrame(VoLayer, VoChn, &stOverlayFrame, 1000);
			if (s32Ret != CVI_SUCCESS) {
				printf("CVI_VO_SendFrame faile\n");
			}
		}
		frm_cnt++;
		CVI_VPSS_ReleaseChnFrame(VpssGrp1, VpssChn, &stOverlayFrame);
#ifdef SHOW_STATISTICS_2
		gettimeofday(&tv2, NULL);
		int curr_ms =
			(tv2.tv_sec - tv1.tv_sec) * 1000 + (tv2.tv_usec/1000) - (tv1.tv_usec/1000);
		printf("CVI_VO_SendFrame delta %d ms try %d times\n", curr_ms, retry);
		tv1 = tv2;
#endif
		retry = 0;
	}
	printf("thread_send_vo exit\n");
	return NULL;
}

VB_POOL g_ahLocalPicVbPool[MAX_VDEC_NUM] = { [0 ...(MAX_VDEC_NUM - 1)] = VB_INVALID_POOLID };

CVI_S32 _VDEC_InitVBPool(VDEC_CHN ChnNum, SAMPLE_VDEC_ATTR *pastSampleVdec)
{
	CVI_S32 s32Ret = CVI_SUCCESS, i;
	CVI_U32 u32BlkSize;
	VB_CONFIG_S stVbConf;

	SAMPLE_VDEC_BUF astSampleVdecBuf[VDEC_MAX_CHN_NUM];
	VB_POOL_CONFIG_S stVbPoolCfg;

	memset(&stVbConf, 0, sizeof(VB_CONFIG_S));
	stVbConf.u32MaxPoolCnt = ChnNum;

	for (i = 0; i < ChnNum; i++) {
		u32BlkSize =
			VDEC_GetPicBufferSize(pastSampleVdec[i].enType, pastSampleVdec[i].u32Width,
					      pastSampleVdec[i].u32Height,
					      pastSampleVdec[i].enPixelFormat, DATA_BITWIDTH_8,
					      COMPRESS_MODE_NONE);
		stVbConf.astCommPool[i].u32BlkSize = u32BlkSize;
		stVbConf.astCommPool[i].u32BlkCnt = pastSampleVdec[i].u32FrameBufCnt;
		printf("VDec Init Pool[VdecChn%d], u32BlkSize = %d, u32BlkCnt = %d\n", i,
			   stVbConf.astCommPool[i].u32BlkSize, stVbConf.astCommPool[i].u32BlkCnt);

		astSampleVdecBuf[i].u32PicBufSize = stVbConf.astCommPool[i].u32BlkSize;
	}

	for (i = 0; i < ChnNum; i++) {
		if ((astSampleVdecBuf[i].u32PicBufSize != 0) && (pastSampleVdec[i].u32FrameBufCnt != 0)) {
			memset(&stVbPoolCfg, 0, sizeof(VB_POOL_CONFIG_S));
			stVbPoolCfg.u32BlkSize	= astSampleVdecBuf[i].u32PicBufSize;
			stVbPoolCfg.u32BlkCnt	= pastSampleVdec[i].u32FrameBufCnt;
			stVbPoolCfg.enRemapMode = VB_REMAP_MODE_NONE;

			g_ahLocalPicVbPool[i] = CVI_VB_CreatePool(&stVbPoolCfg);
			printf("CVI_VB_CreatePool : %d, u32BlkSize=0x%x, u32BlkCnt=%d\n",
				g_ahLocalPicVbPool[i], stVbPoolCfg.u32BlkSize, stVbPoolCfg.u32BlkCnt);

			if (g_ahLocalPicVbPool[i] == VB_INVALID_POOLID) {
				CVI_VDEC_ERR("CVI_VB_CreatePool Fail\n");
				return CVI_FAILURE;
			}
		}
	}

	return s32Ret;
}

CVI_VOID _VDEC_ExitVBPool(CVI_VOID)
{
	CVI_S32 i, s32Ret;

	for (i = MAX_VDEC_NUM-1; i >= 0; i--) {
		if (g_ahLocalPicVbPool[i] != VB_INVALID_POOLID) {
			CVI_VDEC_TRACE("CVI_VB_DestroyPool : %d\n", g_ahLocalPicVbPool[i]);

			s32Ret =  CVI_VB_DestroyPool(g_ahLocalPicVbPool[i]);
			if (s32Ret != CVI_SUCCESS) {
				CVI_VDEC_ERR("CVI_VB_DestroyPool : %d fail!\n", g_ahLocalPicVbPool[i]);
			}

			g_ahLocalPicVbPool[i] = VB_INVALID_POOLID;
		}
	}
}

CVI_S32 start_vdec(SAMPLE_VDEC_PARAM_S *pVdecParam)
{
	VDEC_CHN_PARAM_S stChnParam = {0};
	VDEC_MOD_PARAM_S stModParam;
	CVI_S32 s32Ret = CVI_SUCCESS;
	VDEC_CHN VdecChn = pVdecParam->VdecChn;

	CVI_VDEC_GetModParam(&stModParam);
	stModParam.enVdecVBSource = VB_SOURCE_USER;
	CVI_VDEC_SetModParam(&stModParam);

	s32Ret = CVI_VDEC_CreateChn(VdecChn, &pVdecParam->stChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VDEC_CreateChn chn[%d] failed for %#x!\n", pVdecParam->VdecChn, s32Ret);
		return s32Ret;
	}

	{
		VDEC_CHN_POOL_S stPool;

		stPool.hPicVbPool = g_ahLocalPicVbPool[VdecChn];
		stPool.hTmvVbPool = VB_INVALID_POOLID;

		CVI_VDEC_AttachVbPool(VdecChn, &stPool);
	}

	s32Ret = CVI_VDEC_GetChnParam(VdecChn, &stChnParam);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VDEC_GetChnParam chn[%d] failed for %#x!\n", pVdecParam->VdecChn, s32Ret);
		return s32Ret;
	}
	stChnParam.enPixelFormat = PIXEL_FORMAT_YUV_PLANAR_420;
	stChnParam.u32DisplayFrameNum = 3;
	s32Ret = CVI_VDEC_SetChnParam(VdecChn, &stChnParam);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VDEC_SetChnParam chn[%d] failed for %#x!\n", pVdecParam->VdecChn, s32Ret);
		return s32Ret;
	}

	s32Ret = CVI_VDEC_StartRecvStream(VdecChn);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VDEC_StartRecvStream chn[%d] failed for %#x!\n", pVdecParam->VdecChn, s32Ret);
		return s32Ret;
	}

	return CVI_SUCCESS;
}


CVI_VOID stop_vdec(SAMPLE_VDEC_PARAM_S *pVdecParam)
{
	CVI_S32 s32Ret = CVI_SUCCESS;

	s32Ret = CVI_VDEC_StopRecvStream(pVdecParam->VdecChn);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VDEC_StopRecvStream chn[%d] failed for %#x!\n", pVdecParam->VdecChn, s32Ret);
	}
	s32Ret = CVI_VDEC_DestroyChn(pVdecParam->VdecChn);
	if (s32Ret != CVI_SUCCESS) {
		printf("CVI_VDEC_DestroyChn chn[%d] failed for %#x!\n", pVdecParam->VdecChn, s32Ret);
	}
}


CVI_S32 start_thread(SAMPLE_VDEC_CONFIG_S *pstVdecCfg)
{
	CVI_S32 s32Ret = CVI_SUCCESS;
	struct sched_param param;
	pthread_attr_t attr;

	param.sched_priority = 80;
	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

	for (int i = 0; i < pstVdecCfg->s32ChnNum; i++) {
		s32Ret = pthread_create(&pstVdecCfg->astVdecParam[i].vdec_thread, &attr,
			thread_vdec_send_stream, (void *)&pstVdecCfg->astVdecParam[i]);
		if (s32Ret != 0) {
			return CVI_FAILURE;
		}
		usleep(100000);
	}
	usleep(100000);
	s32Ret = pthread_create(&send_vo_thread, &attr, thread_send_vo, (void *)pstVdecCfg);
	if (s32Ret != 0) {
		return CVI_FAILURE;
	}

	return CVI_SUCCESS;
}

CVI_VOID stop_thread(SAMPLE_VDEC_CONFIG_S *pstVdecCfg)
{
	pstVdecCfg->astVdecParam[0].stop_thread = CVI_TRUE;
	if (send_vo_thread != 0)
		pthread_join(send_vo_thread, NULL);

	for (int i = 0; i < pstVdecCfg->s32ChnNum; i++) {
		pstVdecCfg->astVdecParam[i].stop_thread = CVI_TRUE;
		if (pstVdecCfg->astVdecParam[i].vdec_thread != 0)
			pthread_join(pstVdecCfg->astVdecParam[i].vdec_thread, NULL);
	}

}

PAYLOAD_TYPE_E find_file_type(const char *filename, int filelen)
{
	if (strcmp(filename + filelen - 3, "265") == 0) {
		return PT_H265;
	} else if (strcmp(filename + filelen - 3, "264") == 0) {
		return PT_H264;
	} else if (strcmp(filename + filelen - 3, "jpg") == 0) {
		return PT_JPEG;
	} else if (strcmp(filename + filelen - 3, "mjp") == 0) {
		return PT_MJPEG;
	} else {
		return PT_BUTT;
	}
}
int decoding_file_num;
CVI_S32 SAMPLE_VPSS_Overlay_4vdec(CVI_VOID)
{
	COMPRESS_MODE_E    enCompressMode   = COMPRESS_MODE_NONE;

	VB_CONFIG_S        stVbConf;
	CVI_U32	       u32BlkSize;
	SIZE_S stSize;
	CVI_S32 s32Ret = CVI_SUCCESS;
	int filelen;

	stSize.u32Width = AIKB_VIDEO_WIDTH;
	stSize.u32Height = AIKB_VIDEO_HEIGHT;

	/************************************************
	 * step3:  Init SYS and common VB
	 ************************************************/
	memset(&stVbConf, 0, sizeof(VB_CONFIG_S));
	stVbConf.u32MaxPoolCnt		= 1;

	u32BlkSize = COMMON_GetPicBufferSize(stSize.u32Width, stSize.u32Height, SAMPLE_PIXEL_FORMAT, DATA_BITWIDTH_8
					    , enCompressMode, DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkSize	= u32BlkSize;
	stVbConf.astCommPool[0].u32BlkCnt	= 5;
	stVbConf.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	SAMPLE_PRT("common pool[0] BlkSize %d\n", u32BlkSize);

	s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("system init failed with %#x\n", s32Ret);
		return -1;
	}

	/************************************************
	 * step5:  Init VPSS
	 ************************************************/
	VPSS_GRP_ATTR_S    stVpssGrpAttr = {0};
	VPSS_CHN           VpssChn        = VPSS_CHN0;
	CVI_BOOL           abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {0};
	VPSS_CHN_ATTR_S    astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM] = {0};

	stVpssGrpAttr.stFrameRate.s32SrcFrameRate    = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate    = -1;
	stVpssGrpAttr.enPixelFormat                  = PIXEL_FORMAT_YUV_PLANAR_420;
	stVpssGrpAttr.u32MaxW                        = stSize.u32Width;
	stVpssGrpAttr.u32MaxH                        = stSize.u32Height;
	stVpssGrpAttr.u8VpssDev                      = 0;

	astVpssChnAttr[VpssChn].u32Width                    = AIKB_VIDEO_WIDTH;
	astVpssChnAttr[VpssChn].u32Height                   = AIKB_VIDEO_HEIGHT;
	astVpssChnAttr[VpssChn].enVideoFormat               = VIDEO_FORMAT_LINEAR;
	astVpssChnAttr[VpssChn].enPixelFormat               = SAMPLE_PIXEL_FORMAT;
	astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = 30;
	astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = 30;
	astVpssChnAttr[VpssChn].u32Depth                    = 0;
	astVpssChnAttr[VpssChn].bMirror                     = CVI_FALSE;
	astVpssChnAttr[VpssChn].bFlip                       = CVI_FALSE;
	astVpssChnAttr[VpssChn].stAspectRatio.enMode        = ASPECT_RATIO_NONE;

	/*overlay vpss grp*/
	VPSS_GRP	VpssGrp1	= 1;

	abChnEnable[0] = CVI_TRUE;
	s32Ret = SAMPLE_COMM_VPSS_Init(VpssGrp1, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("init vpss group failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_VPSS_Start(VpssGrp1, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("start vpss group failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}

	SAMPLE_VO_CONFIG_S stVoConfig;

	if (is_using_vo) {
	/************************************************
	 * step6:  Init VO
	 ************************************************/
	RECT_S stDefDispRect  = {0, 0, AIKB_PANEL_WIDTH, AIKB_PANEL_HEIGHT};
	SIZE_S stDefImageSize = {AIKB_PANEL_WIDTH, AIKB_PANEL_HEIGHT};
	VO_SYNC_INFO_S stAikbPanelSyncInfo = {
		.bSynm = 1,
		.bIop = 1,
		.u16FrameRate = 60,
		.u16Vact = AIKB_PANEL_HEIGHT,
		.u16Vbb = 20,
		.u16Vfb = 20,
		.u16Hact = AIKB_PANEL_WIDTH,
		.u16Hbb = 50,
		.u16Hfb = 50,
		.u16Hpw = 10,
		.u16Vpw = 10,
		.bIdv = 0,
		.bIhs = 0,
		.bIvs = 0,
	};
	s32Ret = SAMPLE_COMM_VO_GetDefConfig(&stVoConfig);
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "SAMPLE_COMM_VO_GetDefConfig failed with %#x\n", s32Ret);
		return s32Ret;
	}

	stVoConfig.VoDev		 = 0;
	stVoConfig.stVoPubAttr.enIntfType  = VO_INTF_MIPI;
	stVoConfig.stVoPubAttr.enIntfSync  = VO_OUTPUT_USER;
	stVoConfig.stVoPubAttr.stSyncInfo  = stAikbPanelSyncInfo;
	stVoConfig.stDispRect	 = stDefDispRect;
	stVoConfig.stImageSize	 = stDefImageSize;
	stVoConfig.enPixFormat	 = SAMPLE_PIXEL_FORMAT;
	stVoConfig.enVoMode		 = VO_MODE_1MUX;

	s32Ret = SAMPLE_COMM_VO_StartVO(&stVoConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VO_StartVO failed with %#x\n", s32Ret);
		return s32Ret;
	}

	}

	SAMPLE_VDEC_CONFIG_S stVdecCfg = {0};
	SAMPLE_VDEC_PARAM_S *pVdecChn[MAX_VDEC_NUM];

	pVdecChn[0] = &stVdecCfg.astVdecParam[0];
	pVdecChn[1] = &stVdecCfg.astVdecParam[1];
	pVdecChn[2] = &stVdecCfg.astVdecParam[2];
	pVdecChn[3] = &stVdecCfg.astVdecParam[3];

	stVdecCfg.s32ChnNum = decoding_file_num;

	pVdecChn[0]->VdecChn = 0;
	pVdecChn[0]->stop_thread = CVI_FALSE;
	filelen = snprintf(pVdecChn[0]->decode_file_name,
			   sizeof(pVdecChn[0]->decode_file_name), "%s",
			   (char *)s_h264file[0]);
	pVdecChn[0]->stChnAttr.enType = find_file_type(pVdecChn[0]->decode_file_name, filelen);
	pVdecChn[0]->stChnAttr.enMode = VIDEO_MODE_FRAME;
	pVdecChn[0]->stChnAttr.u32PicWidth = AIKB_VIDEO_WIDTH;
	pVdecChn[0]->stChnAttr.u32PicHeight = AIKB_VIDEO_HEIGHT;
	pVdecChn[0]->stChnAttr.u32FrameBufCnt = 3;
	pVdecChn[0]->stChnAttr.u32StreamBufSize = AIKB_VIDEO_WIDTH * AIKB_VIDEO_HEIGHT;
	pVdecChn[0]->stDispRect.s32X = 0;
	pVdecChn[0]->stDispRect.s32Y = 0;
	pVdecChn[0]->stDispRect.u32Width = 640;
	pVdecChn[0]->stDispRect.u32Height = 360;
	if (decoding_file_num == 1)
		set_single_video_display_rect(pVdecChn[0]);

	for (int i = 1; i < stVdecCfg.s32ChnNum; i++) {
		memcpy(pVdecChn[i], pVdecChn[0], sizeof(SAMPLE_VDEC_PARAM_S));
		pVdecChn[i]->VdecChn = i;
		filelen = snprintf(pVdecChn[i]->decode_file_name,
				    sizeof(pVdecChn[i]->decode_file_name), "%s",
				    (char *)s_h264file[i]);
		pVdecChn[i]->stChnAttr.enType = find_file_type(pVdecChn[i]->decode_file_name, filelen);
	}

#define vbMaxFrmNum 8
	////////////////////////////////////////////////////
	// init user VB(for VDEC)
	////////////////////////////////////////////////////
	SAMPLE_VDEC_ATTR astSampleVdec[VDEC_MAX_CHN_NUM];

	for (int i = 0; i < stVdecCfg.s32ChnNum; i++) {
		astSampleVdec[i].enType = pVdecChn[i]->stChnAttr.enType;
		astSampleVdec[i].u32Width = AIKB_VIDEO_WIDTH;
		astSampleVdec[i].u32Height = AIKB_VIDEO_HEIGHT;

		astSampleVdec[i].enMode = VIDEO_MODE_FRAME;
		astSampleVdec[i].stSampleVdecVideo.enDecMode = VIDEO_DEC_MODE_IP;
		astSampleVdec[i].stSampleVdecVideo.enBitWidth = DATA_BITWIDTH_8;
		astSampleVdec[i].stSampleVdecVideo.u32RefFrameNum = 2;
		astSampleVdec[i].u32DisplayFrameNum = 2;
		astSampleVdec[i].enPixelFormat = PIXEL_FORMAT_YUV_PLANAR_420;
		astSampleVdec[i].u32FrameBufCnt =
			(astSampleVdec[i].enType == PT_JPEG || astSampleVdec[i].enType == PT_MJPEG) ? 3 : vbMaxFrmNum;
	}

	s32Ret = _VDEC_InitVBPool(stVdecCfg.s32ChnNum, &astSampleVdec[0]);
	if (s32Ret != CVI_SUCCESS) {
		CVI_VDEC_ERR("SAMPLE_COMM_VDEC_InitVBPool fail\n");
	}

	for (int i = 0; i < stVdecCfg.s32ChnNum; i++) {
		start_vdec(pVdecChn[i]);
	}

	if (stVdecCfg.s32ChnNum == 1)
		start_video_ctrl_thread();
	start_thread(&stVdecCfg);

	printf("---------------press Ctrl+C to exit!---------------\n");
	while (is_running)
		pause();

	stop_thread(&stVdecCfg);
	printf("stop thread\n");
	for (int i = stVdecCfg.s32ChnNum; i > 0; i--) {
		stop_vdec(pVdecChn[i-1]);
	}
	if (is_using_vo) {
		SAMPLE_COMM_VO_StopVO(&stVoConfig);
	}
	SAMPLE_COMM_VPSS_Stop(VpssGrp1, abChnEnable);

	_VDEC_ExitVBPool();

	SAMPLE_COMM_SYS_Exit();
	return s32Ret;
}

CVI_VOID SAMPLE_VIO_HandleSig(CVI_S32 signo)
{
	if (signo == SIGALRM) {
		char buf[4] = "qoo\n";

		write(alarm_sockets[0], buf, 4);
	} else if (SIGINT == signo || SIGTERM == signo) {
		//todo for release
		SAMPLE_PRT("Program terminated, press Enter to exit\n");
		is_running = 0;
	}
}

CVI_VOID SAMPLE_VIO_Usage(CVI_CHAR *sPrgNm)
{
	printf("Usage : %s <index> <h26xfile1> <h26xfile2> <h26xfile3> <h26xfile4>\n", sPrgNm);
	printf("h26xfile extension: .265 or .264\n");
	printf("index:\n");
	printf("\t 0)4vdec + vpss w/o vo\n");
	printf("\t 1)4vdec + vpss + vo, Multi-channel display.\n");
	printf("\t P.S. run sample_dsi first to init panel.\n");
}

unsigned int setalarm(unsigned int msec)
{
	struct itimerval old, new;

	new.it_interval.tv_usec = msec*1000;
	new.it_interval.tv_sec = 0;
	new.it_value.tv_usec = 0;
	new.it_value.tv_sec = 1;
	if (setitimer(ITIMER_REAL, &new, &old) < 0)
		return 0;
	else
		return old.it_value.tv_sec;
}

int main(int argc, char *argv[])
{
	CVI_S32 s32Ret = CVI_FAILURE;
	CVI_S32 s32Index;

	decoding_file_num = argc - 2;
	if (decoding_file_num < 1 || decoding_file_num > MAX_VDEC_NUM) {
		SAMPLE_VIO_Usage(argv[0]);
		return CVI_FAILURE;
	}
	s32Index = atoi(argv[1]);
	for (int i = 0; i < decoding_file_num; i++)
		s_h264file[i] = argv[i + 2];
	signal(SIGINT, SAMPLE_VIO_HandleSig);
	signal(SIGALRM, SAMPLE_VIO_HandleSig);
	signal(SIGTERM, SAMPLE_VIO_HandleSig);

	socketpair(AF_UNIX, SOCK_STREAM, 0, alarm_sockets);
	setalarm(40);

	switch (s32Index) {
	case 0:
		is_using_vo = false;
		s32Ret = SAMPLE_VPSS_Overlay_4vdec();
		break;
	case 1:
		s32Ret = SAMPLE_VPSS_Overlay_4vdec();
		break;

	default:
		SAMPLE_PRT("the index %d is invaild!\n", s32Index);
		SAMPLE_VIO_Usage(argv[0]);
		return CVI_FAILURE;
	}

	if (s32Ret == CVI_SUCCESS)
		SAMPLE_PRT("sample_vio exit success!\n");
	else
		SAMPLE_PRT("sample_vio exit abnormally!\n");

	close(alarm_sockets[0]);
	close(alarm_sockets[1]);
	return s32Ret;
}
