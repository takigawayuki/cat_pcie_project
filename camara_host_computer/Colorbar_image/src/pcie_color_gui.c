#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "colorbar_pcie_rx.h"

#define STATUS_BUSY            (1u << 0)
#define STATUS_DONE            (1u << 1)
#define STATUS_IDLE            (1u << 2)
#define STATUS_FIFO_OVERFLOW   (1u << 11)
#define STATUS_FIFO_UNDERFLOW  (1u << 12)

#define GUI_WINDOW_WIDTH 1920
#define GUI_WINDOW_HEIGHT 1080
#define GUI_PREVIEW_WIDTH 1440
#define GUI_PREVIEW_HEIGHT 810
#define GUI_SIDE_WIDTH 450
#define GUI_LOG_PATH "/tmp/colorbar_pcie_gui.log"

#define PROJECT_ROOT_DEFAULT "/home/cat/cat_pcie_project"
#define INFER_PYTHON_DEFAULT "/home/cat/miniconda3/envs/fenqusai/bin/python"
#define INFER_SCRIPT_REL "test/infer_rknn_plate.py"
#define YOLO_MODEL_REL "model/best.rknn"
#define LPR_MODEL_REL "model/lprnet_unified_p7_yolo_crop_adapt_fp.rknn"
#define PROVINCE_MODEL_REL "model/province_classifier_ft_p8_fp.rknn"
#define INFER_INPUT_PATH "/tmp/colorbar_pcie_plate_frame.png"
#define INFER_OUTPUT_DIR "/tmp/colorbar_pcie_plate_infer"
#define INFER_VIS_PATH INFER_OUTPUT_DIR "/colorbar_pcie_plate_frame_vis.jpg"

struct app_state;

struct capture_result {
    struct app_state *app;
    gboolean ok;
    char message[512];
    struct colorbar_status_info status;
    struct colorbar_frame_info frame;
    double elapsed_ms;
};

struct infer_result {
    struct app_state *app;
    gboolean ok;
    char message[1024];
    char plate_text[128];
    int detection_count;
    int plate_count;
    double total_ms;
};

struct app_state {
    GtkWidget *window;
    GtkWidget *image;
    GtkWidget *connect_button;
    GtkWidget *capture_button;
    GtkWidget *continuous_button;
    GtkWidget *stop_button;
    GtkWidget *save_button;
    GtkWidget *load_model_button;
    GtkWidget *infer_button;
    GtkWidget *capture_infer_button;
    GtkWidget *endian_combo;
    GtkWidget *log_view;
    GtkTextBuffer *log_buffer;

    GtkWidget *frame_label;
    GtkWidget *fps_label;
    GtkWidget *dma_addr_label;
    GtkWidget *active_addr_label;
    GtkWidget *active_len_label;
    GtkWidget *bytes_sent_label;
    GtkWidget *done_label;
    GtkWidget *idle_label;
    GtkWidget *busy_label;
    GtkWidget *overflow_label;
    GtkWidget *underflow_label;
    GtkWidget *model_status_label;
    GtkWidget *plate_result_label;
    GtkWidget *detection_count_label;
    GtkWidget *inference_ms_label;

    int dev_fd;
    gboolean connected;
    gboolean capture_running;
    gboolean infer_running;
    gboolean continuous;
    gboolean infer_after_capture;
    gboolean models_ready;
    gboolean big_endian_rgb565;
    uint32_t frame_counter;
    double fps;
    struct timeval last_frame_time;
    struct colorbar_status_info last_status;

    uint8_t *rgb565;
    uint8_t *rgb888;
    size_t rgb565_size;
    size_t rgb888_size;
};

static void append_log(struct app_state *app, const char *fmt, ...)
{
    char line[1024];
    GtkTextIter end;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    gtk_text_buffer_insert(app->log_buffer, &end, line, -1);
    gtk_text_buffer_insert(app->log_buffer, &end, "\n", -1);

    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->log_view), &end, 0.0, FALSE, 0.0, 1.0);

    FILE *fp = fopen(GUI_LOG_PATH, "a");
    if (fp) {
        fprintf(fp, "%s\n", line);
        fclose(fp);
    }
}

static const char *bit_text(uint32_t status, uint32_t bit)
{
    return (status & bit) ? "1" : "0";
}

static void set_label_text(GtkWidget *label, const char *fmt, ...)
{
    char text[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(label), text);
}

static const char *project_root(void)
{
    const char *root = g_getenv("COLORBAR_PROJECT_ROOT");

    if (root && root[0] != '\0')
        return root;
    return PROJECT_ROOT_DEFAULT;
}

static const char *infer_python(void)
{
    const char *python = g_getenv("COLORBAR_INFER_PYTHON");

    if (python && python[0] != '\0')
        return python;
    return INFER_PYTHON_DEFAULT;
}

static char *project_path(const char *rel)
{
    return g_build_filename(project_root(), rel, NULL);
}

static gboolean path_is_regular(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static gboolean check_python_infer_deps(struct app_state *app, const char *python, gboolean verbose)
{
    char *q_python = g_shell_quote(python);
    char *cmd;
    char *stdout_text = NULL;
    char *stderr_text = NULL;
    GError *err = NULL;
    int exit_status = 0;
    gboolean ok = TRUE;

    cmd = g_strdup_printf("%s -c \"import cv2, numpy, rknnlite.api, ruamel.yaml\"",
                          q_python);
    if (!g_spawn_command_line_sync(cmd, &stdout_text, &stderr_text, &exit_status, &err)) {
        append_log(app, "check inference deps failed: %s", err ? err->message : "unknown error");
        ok = FALSE;
    } else if (exit_status != 0) {
        append_log(app, "inference deps missing: python=%s status=%d stderr=%.500s stdout=%.200s",
                   python, exit_status, stderr_text ? stderr_text : "", stdout_text ? stdout_text : "");
        ok = FALSE;
    } else if (verbose) {
        append_log(app, "inference deps ok: python=%s", python);
    }

    if (err)
        g_error_free(err);
    g_free(q_python);
    g_free(cmd);
    g_free(stdout_text);
    g_free(stderr_text);
    return ok;
}

static gboolean ensure_models_ready(struct app_state *app, gboolean verbose)
{
    char *script = project_path(INFER_SCRIPT_REL);
    char *yolo = project_path(YOLO_MODEL_REL);
    char *lpr = project_path(LPR_MODEL_REL);
    char *province = project_path(PROVINCE_MODEL_REL);
    const char *python = infer_python();
    gboolean ok = TRUE;

    if (!path_is_regular(python)) {
        append_log(app, "missing inference python: %s", python);
        ok = FALSE;
    }
    if (!path_is_regular(script)) {
        append_log(app, "missing infer script: %s", script);
        ok = FALSE;
    }
    if (!path_is_regular(yolo)) {
        append_log(app, "missing YOLO model: %s", yolo);
        ok = FALSE;
    }
    if (!path_is_regular(lpr)) {
        append_log(app, "missing LPR model: %s", lpr);
        ok = FALSE;
    }
    if (!path_is_regular(province)) {
        append_log(app, "missing province model: %s", province);
        ok = FALSE;
    }
    if (ok && !check_python_infer_deps(app, python, verbose))
        ok = FALSE;

    app->models_ready = ok;
    if (app->model_status_label)
        set_label_text(app->model_status_label, "%s", ok ? "ready" : "missing");
    if (verbose && ok)
        append_log(app, "models ready: YOLO + LPRNet + province verifier, python=%s", python);

    g_free(script);
    g_free(yolo);
    g_free(lpr);
    g_free(province);
    return ok;
}

static gboolean save_current_frame_png(struct app_state *app, const char *path)
{
    GdkPixbuf *pixbuf;
    GError *err = NULL;
    gboolean ok;

    if (!app->rgb888 || app->frame_counter == 0) {
        append_log(app, "no captured frame for inference");
        return FALSE;
    }

    pixbuf = gdk_pixbuf_new_from_data(app->rgb888, GDK_COLORSPACE_RGB, FALSE, 8,
                                      COLORBAR_WIDTH, COLORBAR_HEIGHT,
                                      COLORBAR_WIDTH * 3, NULL, NULL);
    ok = gdk_pixbuf_save(pixbuf, path, "png", &err, NULL);
    g_object_unref(pixbuf);
    if (!ok) {
        append_log(app, "save inference input failed: %s", err ? err->message : "unknown error");
        if (err)
            g_error_free(err);
        return FALSE;
    }
    return TRUE;
}

static const char *find_json_payload(const char *text)
{
    const char *p = text;
    const char *last = NULL;

    while (p && *p) {
        const char *hit = strstr(p, "{\"image\"");
        if (!hit)
            break;
        last = hit;
        p = hit + 1;
    }
    return last;
}

static int json_count_objects_in_array(const char *json, const char *key)
{
    char pattern[64];
    const char *p;
    const char *end;
    int count = 0;

    snprintf(pattern, sizeof(pattern), "\"%s\": [", key);
    p = strstr(json, pattern);
    if (!p)
        return 0;
    p = strchr(p, '[');
    end = strchr(p, ']');
    if (!p || !end || end <= p)
        return 0;
    while ((p = strchr(p, '{')) && p < end) {
        count++;
        p++;
    }
    return count;
}

static gboolean json_extract_string_after(const char *json, const char *start, const char *key,
                                          char *out, size_t out_size)
{
    char pattern[64];
    const char *p;
    const char *q;
    size_t len;

    if (!start)
        start = json;
    snprintf(pattern, sizeof(pattern), "\"%s\": ", key);
    p = strstr(start, pattern);
    if (!p)
        return FALSE;
    p += strlen(pattern);
    if (*p != '\"')
        return FALSE;
    p++;
    q = strchr(p, '\"');
    if (!q)
        return FALSE;
    len = (size_t)(q - p);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return TRUE;
}

static gboolean json_extract_number(const char *json, const char *key, double *value)
{
    char pattern[64];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\": ", key);
    p = strstr(json, pattern);
    if (!p)
        return FALSE;
    p += strlen(pattern);
    *value = g_ascii_strtod(p, NULL);
    return TRUE;
}

static void update_status_labels(struct app_state *app, const struct colorbar_status_info *st)
{
    uint32_t status = st->status;

    app->last_status = *st;
    set_label_text(app->frame_label, "%u", app->frame_counter);
    set_label_text(app->fps_label, "%.2f", app->fps);
    set_label_text(app->dma_addr_label, "0x%08x", st->dma_addr_low);
    set_label_text(app->active_addr_label, "0x%08x", st->active_addr);
    set_label_text(app->active_len_label, "%u", st->active_len);
    set_label_text(app->bytes_sent_label, "%u / 0x%08x", st->bytes_sent, st->bytes_sent);
    set_label_text(app->done_label, "%s", bit_text(status, STATUS_DONE));
    set_label_text(app->idle_label, "%s", bit_text(status, STATUS_IDLE));
    set_label_text(app->busy_label, "%s", bit_text(status, STATUS_BUSY));
    set_label_text(app->overflow_label, "%s", bit_text(status, STATUS_FIFO_OVERFLOW));
    set_label_text(app->underflow_label, "%s", bit_text(status, STATUS_FIFO_UNDERFLOW));
}

static int read_full(int fd, uint8_t *data, size_t len)
{
    while (len > 0) {
        ssize_t got = read(fd, data, len);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0) {
            errno = EIO;
            return -1;
        }
        data += got;
        len -= (size_t)got;
    }
    return 0;
}

static void rgb565_to_rgb888(const uint8_t *src, uint8_t *dst, gboolean big_endian)
{
    size_t pixels = (size_t)COLORBAR_WIDTH * COLORBAR_HEIGHT;
    size_t i;

    for (i = 0; i < pixels; i++) {
        uint16_t v;
        uint8_t r5, g6, b5;

        if (big_endian)
            v = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
        else
            v = (uint16_t)src[i * 2] | ((uint16_t)src[i * 2 + 1] << 8);

        r5 = (uint8_t)((v >> 11) & 0x1f);
        g6 = (uint8_t)((v >> 5) & 0x3f);
        b5 = (uint8_t)(v & 0x1f);

        dst[i * 3] = (uint8_t)((r5 << 3) | (r5 >> 2));
        dst[i * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        dst[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    }
}

static gboolean query_status(struct app_state *app, struct colorbar_status_info *status)
{
    memset(status, 0, sizeof(*status));
    if (!app->connected || app->dev_fd < 0)
        return FALSE;
    return ioctl(app->dev_fd, COLORBAR_IOC_GET_STATUS, status) == 0;
}

static void set_buttons_idle(struct app_state *app)
{
    gtk_widget_set_sensitive(app->connect_button, !app->capture_running);
    gtk_widget_set_sensitive(app->capture_button, app->connected && !app->capture_running);
    gtk_widget_set_sensitive(app->continuous_button, app->connected && !app->capture_running);
    gtk_widget_set_sensitive(app->stop_button, app->connected);
    gtk_widget_set_sensitive(app->save_button, app->rgb888 != NULL && app->frame_counter > 0);
    gtk_widget_set_sensitive(app->load_model_button, !app->infer_running);
    gtk_widget_set_sensitive(app->infer_button, app->models_ready && app->frame_counter > 0 && !app->capture_running && !app->continuous && !app->infer_running);
    gtk_widget_set_sensitive(app->capture_infer_button, app->connected && app->models_ready && !app->capture_running && !app->continuous && !app->infer_running);
}

static void set_buttons_capture(struct app_state *app)
{
    gtk_widget_set_sensitive(app->connect_button, FALSE);
    gtk_widget_set_sensitive(app->capture_button, FALSE);
    gtk_widget_set_sensitive(app->continuous_button, FALSE);
    gtk_widget_set_sensitive(app->stop_button, TRUE);
    gtk_widget_set_sensitive(app->save_button, FALSE);
    gtk_widget_set_sensitive(app->load_model_button, !app->infer_running);
    gtk_widget_set_sensitive(app->infer_button, FALSE);
    gtk_widget_set_sensitive(app->capture_infer_button, FALSE);
}

static gboolean start_capture_idle(gpointer data);

static void update_image_preview(struct app_state *app)
{
    GdkPixbuf *native;
    GdkPixbuf *scaled;

    native = gdk_pixbuf_new_from_data(app->rgb888, GDK_COLORSPACE_RGB, FALSE, 8,
                                      COLORBAR_WIDTH, COLORBAR_HEIGHT,
                                      COLORBAR_WIDTH * 3, NULL, NULL);
    scaled = gdk_pixbuf_scale_simple(native, GUI_PREVIEW_WIDTH, GUI_PREVIEW_HEIGHT,
                                     GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(app->image), scaled);
    g_object_unref(scaled);
    g_object_unref(native);
}


static gboolean infer_done_idle(gpointer data)
{
    struct infer_result *res = data;
    struct app_state *app = res->app;

    app->infer_running = FALSE;
    if (res->ok) {
        GError *err = NULL;
        GdkPixbuf *vis = NULL;
        GdkPixbuf *scaled = NULL;

        set_label_text(app->plate_result_label, "%s", res->plate_text[0] ? res->plate_text : "no valid plate");
        set_label_text(app->detection_count_label, "%d / %d", res->plate_count, res->detection_count);
        set_label_text(app->inference_ms_label, "%.2f", res->total_ms);
        append_log(app, "inference ok: plates=%d detections=%d result=%s total=%.2f ms",
                   res->plate_count, res->detection_count,
                   res->plate_text[0] ? res->plate_text : "no valid plate", res->total_ms);

        if (path_is_regular(INFER_VIS_PATH)) {
            vis = gdk_pixbuf_new_from_file(INFER_VIS_PATH, &err);
            if (vis) {
                scaled = gdk_pixbuf_scale_simple(vis, GUI_PREVIEW_WIDTH, GUI_PREVIEW_HEIGHT,
                                                 GDK_INTERP_BILINEAR);
                gtk_image_set_from_pixbuf(GTK_IMAGE(app->image), scaled);
                append_log(app, "visualization loaded: %s", INFER_VIS_PATH);
                g_object_unref(scaled);
                g_object_unref(vis);
            } else {
                append_log(app, "load visualization failed: %s", err ? err->message : "unknown error");
                if (err)
                    g_error_free(err);
            }
        } else {
            append_log(app, "visualization not found: %s", INFER_VIS_PATH);
        }
    } else {
        set_label_text(app->plate_result_label, "infer failed");
        append_log(app, "inference failed: %s", res->message);
    }

    set_buttons_idle(app);
    g_free(res);
    return FALSE;
}

static gpointer infer_thread(gpointer data)
{
    struct app_state *app = data;
    struct infer_result *res = g_new0(struct infer_result, 1);
    const char *python = infer_python();
    char *script = project_path(INFER_SCRIPT_REL);
    char *yolo = project_path(YOLO_MODEL_REL);
    char *lpr = project_path(LPR_MODEL_REL);
    char *province = project_path(PROVINCE_MODEL_REL);
    char *q_python = g_shell_quote(python);
    char *q_script = g_shell_quote(script);
    char *q_input = g_shell_quote(INFER_INPUT_PATH);
    char *q_yolo = g_shell_quote(yolo);
    char *q_lpr = g_shell_quote(lpr);
    char *q_province = g_shell_quote(province);
    char *q_out = g_shell_quote(INFER_OUTPUT_DIR);
    char *cmd;
    char *stdout_text = NULL;
    char *stderr_text = NULL;
    GError *err = NULL;
    int exit_status = 0;
    const char *json;
    struct timeval t0, t1;

    res->app = app;
    gettimeofday(&t0, NULL);

    g_mkdir_with_parents(INFER_OUTPUT_DIR, 0755);
    unlink(INFER_VIS_PATH);
    cmd = g_strdup_printf("%s %s --image %s --yolo-model %s --lpr-model %s --province-model %s "
                          "--output-dir %s --save-vis --no-export-debug",
                          q_python, q_script, q_input, q_yolo, q_lpr, q_province, q_out);

    if (!g_spawn_command_line_sync(cmd, &stdout_text, &stderr_text, &exit_status, &err)) {
        snprintf(res->message, sizeof(res->message), "spawn failed: %s", err ? err->message : "unknown error");
        if (err)
            g_error_free(err);
        goto out;
    }
    if (exit_status != 0) {
        snprintf(res->message, sizeof(res->message), "python exited status=%d stderr=%.700s stdout=%.200s", exit_status,
                 stderr_text ? stderr_text : "", stdout_text ? stdout_text : "");
        goto out;
    }

    json = stdout_text ? find_json_payload(stdout_text) : NULL;
    if (!json) {
        snprintf(res->message, sizeof(res->message), "no JSON payload; stderr=%.500s stdout=%.400s",
                 stderr_text ? stderr_text : "", stdout_text ? stdout_text : "");
        goto out;
    }

    res->detection_count = json_count_objects_in_array(json, "detections");
    res->plate_count = json_count_objects_in_array(json, "plates");
    if (res->plate_count > 0) {
        const char *plate = strstr(json, "\"plates\": [");
        json_extract_string_after(json, plate, "plate_text", res->plate_text, sizeof(res->plate_text));
    }
    json_extract_number(json, "total_ms", &res->total_ms);
    res->ok = TRUE;
    snprintf(res->message, sizeof(res->message), "ok");

out:
    gettimeofday(&t1, NULL);
    if (res->total_ms <= 0.0)
        res->total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (t1.tv_usec - t0.tv_usec) / 1000.0;
    g_idle_add(infer_done_idle, res);
    g_free(script);
    g_free(yolo);
    g_free(lpr);
    g_free(province);
    g_free(q_python);
    g_free(q_script);
    g_free(q_input);
    g_free(q_yolo);
    g_free(q_lpr);
    g_free(q_province);
    g_free(q_out);
    g_free(cmd);
    g_free(stdout_text);
    g_free(stderr_text);
    return NULL;
}

static gboolean start_inference_current_frame(struct app_state *app)
{
    if (app->continuous) {
        append_log(app, "stop continuous capture before inference");
        return FALSE;
    }
    if (app->infer_running || app->capture_running)
        return FALSE;
    if (!ensure_models_ready(app, FALSE))
        return FALSE;
    if (!save_current_frame_png(app, INFER_INPUT_PATH))
        return FALSE;

    app->infer_running = TRUE;
    set_buttons_idle(app);
    set_label_text(app->plate_result_label, "running...");
    append_log(app, "inference started: %s", INFER_INPUT_PATH);
    g_thread_new("plate-infer", infer_thread, app);
    return TRUE;
}

static gboolean capture_done_idle(gpointer data)
{
    struct capture_result *res = data;
    struct app_state *app = res->app;
    struct timeval now;

    app->capture_running = FALSE;
    update_status_labels(app, &res->status);

    if (res->ok) {
        app->frame_counter = res->frame.frame_counter;
        gettimeofday(&now, NULL);
        if (app->last_frame_time.tv_sec != 0 || app->last_frame_time.tv_usec != 0) {
            double dt = (now.tv_sec - app->last_frame_time.tv_sec) +
                        (now.tv_usec - app->last_frame_time.tv_usec) / 1000000.0;
            if (dt > 0.0001)
                app->fps = 1.0 / dt;
        }
        app->last_frame_time = now;

        update_image_preview(app);
        update_status_labels(app, &res->status);
        append_log(app, "frame %u captured: %u bytes, %.2f ms, %.2f fps",
                   res->frame.frame_counter, res->frame.valid_size,
                   res->elapsed_ms, app->fps);
        if (app->infer_after_capture) {
            app->infer_after_capture = FALSE;
            start_inference_current_frame(app);
        }
    } else {
        app->continuous = FALSE;
        app->infer_after_capture = FALSE;
        append_log(app, "capture stopped: %s", res->message);
        append_log(app, "status=0x%08x active_addr=0x%08x active_len=%u bytes_sent=%u overflow=%s underflow=%s done=%s idle=%s busy=%s",
                   res->status.status, res->status.active_addr, res->status.active_len,
                   res->status.bytes_sent,
                   bit_text(res->status.status, STATUS_FIFO_OVERFLOW),
                   bit_text(res->status.status, STATUS_FIFO_UNDERFLOW),
                   bit_text(res->status.status, STATUS_DONE),
                   bit_text(res->status.status, STATUS_IDLE),
                   bit_text(res->status.status, STATUS_BUSY));
    }

    set_buttons_idle(app);

    if (app->continuous && res->ok)
        g_idle_add(start_capture_idle, app);

    g_free(res);
    return FALSE;
}

static gpointer capture_thread(gpointer data)
{
    struct app_state *app = data;
    struct capture_result *res = g_new0(struct capture_result, 1);
    struct timeval t0, t1;
    struct colorbar_status_info st;

    res->app = app;
    gettimeofday(&t0, NULL);

    if (ioctl(app->dev_fd, COLORBAR_IOC_START) < 0) {
        snprintf(res->message, sizeof(res->message), "START failed: %s", strerror(errno));
        query_status(app, &res->status);
        goto out;
    }

    if (ioctl(app->dev_fd, COLORBAR_IOC_WAIT_FRAME, &res->frame) < 0) {
        snprintf(res->message, sizeof(res->message), "WAIT_FRAME failed: %s", strerror(errno));
        query_status(app, &res->status);
        ioctl(app->dev_fd, COLORBAR_IOC_STOP);
        goto out;
    }

    if (res->frame.valid_size != COLORBAR_FRAME_SIZE) {
        snprintf(res->message, sizeof(res->message), "unexpected frame size: %u", res->frame.valid_size);
        query_status(app, &res->status);
        ioctl(app->dev_fd, COLORBAR_IOC_STOP);
        goto out;
    }

    if (lseek(app->dev_fd, 0, SEEK_SET) < 0) {
        snprintf(res->message, sizeof(res->message), "lseek failed: %s", strerror(errno));
        query_status(app, &res->status);
        ioctl(app->dev_fd, COLORBAR_IOC_STOP);
        goto out;
    }

    if (read_full(app->dev_fd, app->rgb565, COLORBAR_FRAME_SIZE) < 0) {
        snprintf(res->message, sizeof(res->message), "read frame failed: %s", strerror(errno));
        query_status(app, &res->status);
        ioctl(app->dev_fd, COLORBAR_IOC_STOP);
        goto out;
    }

    rgb565_to_rgb888(app->rgb565, app->rgb888, app->big_endian_rgb565);

    if (query_status(app, &st))
        res->status = st;

    gettimeofday(&t1, NULL);
    res->elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                      (t1.tv_usec - t0.tv_usec) / 1000.0;
    res->ok = TRUE;
    snprintf(res->message, sizeof(res->message), "ok");

out:
    g_idle_add(capture_done_idle, res);
    return NULL;
}

static gboolean start_capture_idle(gpointer data)
{
    struct app_state *app = data;

    if (!app->connected || app->capture_running)
        return FALSE;

    app->capture_running = TRUE;
    app->big_endian_rgb565 = gtk_combo_box_get_active(GTK_COMBO_BOX(app->endian_combo)) == 1;
    set_buttons_capture(app);
    g_thread_new("colorbar-capture", capture_thread, app);
    return FALSE;
}


static void on_load_model_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    (void)button;

    ensure_models_ready(app, TRUE);
    set_buttons_idle(app);
}

static void on_infer_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    (void)button;

    start_inference_current_frame(app);
}

static void on_capture_infer_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    (void)button;

    if (app->continuous) {
        append_log(app, "stop continuous capture before capture+infer");
        return;
    }
    if (!ensure_models_ready(app, FALSE)) {
        set_buttons_idle(app);
        return;
    }
    app->continuous = FALSE;
    app->infer_after_capture = TRUE;
    append_log(app, "capture one frame, then run YOLO + LPR");
    start_capture_idle(app);
}

static void on_capture_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    (void)button;
    app->continuous = FALSE;
    start_capture_idle(app);
}

static void on_continuous_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    (void)button;
    app->continuous = TRUE;
    append_log(app, "continuous capture enabled");
    start_capture_idle(app);
}

static void on_stop_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    struct colorbar_status_info st;
    (void)button;

    app->continuous = FALSE;
    if (app->connected && app->dev_fd >= 0 && !app->capture_running) {
        if (ioctl(app->dev_fd, COLORBAR_IOC_SAFE_STOP) < 0)
            append_log(app, "safe stop failed: %s", strerror(errno));
        else
            append_log(app, "safe stop sent");
        if (query_status(app, &st))
            update_status_labels(app, &st);
    } else {
        append_log(app, "stop requested; current frame will finish or timeout first");
    }
    set_buttons_idle(app);
}

static void on_connect_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    struct colorbar_rx_info info;
    struct colorbar_status_info st;
    (void)button;

    if (app->connected) {
        ioctl(app->dev_fd, COLORBAR_IOC_SAFE_STOP);
        ioctl(app->dev_fd, COLORBAR_IOC_FREE_BUFS);
        close(app->dev_fd);
        app->dev_fd = -1;
        app->connected = FALSE;
        append_log(app, "device disconnected");
        gtk_button_set_label(GTK_BUTTON(app->connect_button), "连接设备");
        set_buttons_idle(app);
        return;
    }

    app->dev_fd = open(COLORBAR_DEVICE_PATH, O_RDWR);
    if (app->dev_fd < 0) {
        append_log(app, "open %s failed: %s", COLORBAR_DEVICE_PATH, strerror(errno));
        return;
    }

    if (ioctl(app->dev_fd, COLORBAR_IOC_GET_INFO, &info) < 0) {
        append_log(app, "GET_INFO failed: %s", strerror(errno));
        close(app->dev_fd);
        app->dev_fd = -1;
        return;
    }

    if (ioctl(app->dev_fd, COLORBAR_IOC_ALLOC_BUFS) < 0) {
        append_log(app, "ALLOC_BUFS failed: %s", strerror(errno));
        close(app->dev_fd);
        app->dev_fd = -1;
        return;
    }

    app->connected = TRUE;
    gtk_button_set_label(GTK_BUTTON(app->connect_button), "断开设备");
    append_log(app, "connected: %ux%u RGB565 frame=%u buffer=%u", info.width, info.height,
               info.frame_size, info.buffer_size);
    if (query_status(app, &st))
        update_status_labels(app, &st);
    set_buttons_idle(app);
}

static void on_save_clicked(GtkButton *button, gpointer data)
{
    struct app_state *app = data;
    GtkWidget *dialog;
    gint response;
    (void)button;

    if (!app->rgb888 || app->frame_counter == 0) {
        append_log(app, "no frame to save");
        return;
    }

    dialog = gtk_file_chooser_dialog_new("保存图片", GTK_WINDOW(app->window),
                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                         GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                         GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
                                         NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "frame_single1.png");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GError *err = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(app->rgb888, GDK_COLORSPACE_RGB, FALSE, 8,
                                                     COLORBAR_WIDTH, COLORBAR_HEIGHT,
                                                     COLORBAR_WIDTH * 3, NULL, NULL);
        if (!gdk_pixbuf_save(pixbuf, path, "png", &err, NULL)) {
            append_log(app, "save failed: %s", err ? err->message : "unknown error");
            if (err)
                g_error_free(err);
        } else {
            append_log(app, "saved image: %s", path);
        }
        g_object_unref(pixbuf);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void on_endian_changed(GtkComboBox *combo, gpointer data)
{
    struct app_state *app = data;
    (void)combo;

    app->big_endian_rgb565 = gtk_combo_box_get_active(GTK_COMBO_BOX(app->endian_combo)) == 1;
    if (app->rgb565 && app->rgb888 && app->frame_counter > 0) {
        rgb565_to_rgb888(app->rgb565, app->rgb888, app->big_endian_rgb565);
        update_image_preview(app);
        append_log(app, "RGB565 endian switched to %s", app->big_endian_rgb565 ? "big" : "little");
    }
}

static GtkWidget *make_label_pair(GtkWidget *table, int row, const char *name, GtkWidget **value)
{
    GtkWidget *label = gtk_label_new(name);
    *value = gtk_label_new("-");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(*value), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1,
                     GTK_FILL, GTK_FILL, 6, 2);
    gtk_table_attach(GTK_TABLE(table), *value, 1, 2, row, row + 1,
                     GTK_FILL | GTK_EXPAND, GTK_FILL, 6, 2);
    return *value;
}

static GtkWidget *build_status_table(struct app_state *app)
{
    GtkWidget *table = gtk_table_new(11, 2, FALSE);

    make_label_pair(table, 0, "帧号", &app->frame_label);
    make_label_pair(table, 1, "帧率", &app->fps_label);
    make_label_pair(table, 2, "DMA_ADDR", &app->dma_addr_label);
    make_label_pair(table, 3, "ACTIVE_ADDR", &app->active_addr_label);
    make_label_pair(table, 4, "ACTIVE_LEN", &app->active_len_label);
    make_label_pair(table, 5, "BYTES_SENT", &app->bytes_sent_label);
    make_label_pair(table, 6, "DONE", &app->done_label);
    make_label_pair(table, 7, "IDLE", &app->idle_label);
    make_label_pair(table, 8, "BUSY", &app->busy_label);
    make_label_pair(table, 9, "FIFO_OVERFLOW", &app->overflow_label);
    make_label_pair(table, 10, "FIFO_UNDERFLOW", &app->underflow_label);
    return table;
}


static GtkWidget *build_infer_table(struct app_state *app)
{
    GtkWidget *frame = gtk_frame_new("YOLO + LPR");
    GtkWidget *table = gtk_table_new(4, 2, FALSE);

    make_label_pair(table, 0, "模型", &app->model_status_label);
    make_label_pair(table, 1, "车牌", &app->plate_result_label);
    gtk_label_set_selectable(GTK_LABEL(app->plate_result_label), TRUE);
    make_label_pair(table, 2, "车牌/目标", &app->detection_count_label);
    make_label_pair(table, 3, "推理ms", &app->inference_ms_label);
    gtk_container_add(GTK_CONTAINER(frame), table);
    set_label_text(app->model_status_label, "not loaded");
    set_label_text(app->plate_result_label, "-");
    set_label_text(app->detection_count_label, "-");
    set_label_text(app->inference_ms_label, "-");
    return frame;
}

static void build_ui(struct app_state *app)
{
    GtkWidget *root;
    GtkWidget *toolbar;
    GtkWidget *main_hbox;
    GtkWidget *image_frame;
    GtkWidget *image_align;
    GtkWidget *side_vbox;
    GtkWidget *log_scroll;

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "PCIe Colorbar Receiver");
    gtk_window_set_default_size(GTK_WINDOW(app->window), GUI_WINDOW_WIDTH, GUI_WINDOW_HEIGHT);
    gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);

    root = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(root), 6);
    gtk_container_add(GTK_CONTAINER(app->window), root);

    toolbar = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);

    app->connect_button = gtk_button_new_with_label("连接设备");
    app->capture_button = gtk_button_new_with_label("采集一帧");
    app->continuous_button = gtk_button_new_with_label("连续采集");
    app->stop_button = gtk_button_new_with_label("停止");
    app->save_button = gtk_button_new_with_label("保存图片");
    app->load_model_button = gtk_button_new_with_label("加载模型");
    app->infer_button = gtk_button_new_with_label("识别当前帧");
    app->capture_infer_button = gtk_button_new_with_label("采集并识别");
    app->endian_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->endian_combo), "RGB565 Little Endian");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->endian_combo), "RGB565 Big Endian");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->endian_combo), 0);

    gtk_box_pack_start(GTK_BOX(toolbar), app->connect_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->capture_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->continuous_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->save_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->load_model_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->infer_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->capture_infer_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->endian_combo, FALSE, FALSE, 0);

    main_hbox = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(root), main_hbox, TRUE, TRUE, 0);

    image_frame = gtk_frame_new(NULL);
    gtk_widget_set_size_request(image_frame, GUI_PREVIEW_WIDTH + 8, GUI_PREVIEW_HEIGHT + 8);
    image_align = gtk_alignment_new(0.5, 0.5, 0.0, 0.0);
    app->image = gtk_image_new();
    gtk_widget_set_size_request(app->image, GUI_PREVIEW_WIDTH, GUI_PREVIEW_HEIGHT);
    gtk_container_add(GTK_CONTAINER(image_align), app->image);
    gtk_container_add(GTK_CONTAINER(image_frame), image_align);
    gtk_box_pack_start(GTK_BOX(main_hbox), image_frame, FALSE, FALSE, 0);

    side_vbox = gtk_vbox_new(FALSE, 6);
    gtk_widget_set_size_request(side_vbox, GUI_SIDE_WIDTH, -1);
    gtk_box_pack_start(GTK_BOX(main_hbox), side_vbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(side_vbox), build_status_table(app), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side_vbox), build_infer_table(app), FALSE, FALSE, 0);

    log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(log_scroll, GUI_SIDE_WIDTH, 520);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    app->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->log_view), GTK_WRAP_WORD_CHAR);
    app->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->log_view));
    gtk_container_add(GTK_CONTAINER(log_scroll), app->log_view);
    gtk_box_pack_start(GTK_BOX(side_vbox), log_scroll, TRUE, TRUE, 0);

    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app->connect_button, "clicked", G_CALLBACK(on_connect_clicked), app);
    g_signal_connect(app->capture_button, "clicked", G_CALLBACK(on_capture_clicked), app);
    g_signal_connect(app->continuous_button, "clicked", G_CALLBACK(on_continuous_clicked), app);
    g_signal_connect(app->stop_button, "clicked", G_CALLBACK(on_stop_clicked), app);
    g_signal_connect(app->save_button, "clicked", G_CALLBACK(on_save_clicked), app);
    g_signal_connect(app->load_model_button, "clicked", G_CALLBACK(on_load_model_clicked), app);
    g_signal_connect(app->infer_button, "clicked", G_CALLBACK(on_infer_clicked), app);
    g_signal_connect(app->capture_infer_button, "clicked", G_CALLBACK(on_capture_infer_clicked), app);
    g_signal_connect(app->endian_combo, "changed", G_CALLBACK(on_endian_changed), app);

    set_buttons_idle(app);
}

int main(int argc, char **argv)
{
    struct app_state app;

    memset(&app, 0, sizeof(app));
    app.dev_fd = -1;
    app.rgb565_size = COLORBAR_FRAME_SIZE;
    app.rgb888_size = (size_t)COLORBAR_WIDTH * COLORBAR_HEIGHT * 3;
    app.rgb565 = g_malloc0(app.rgb565_size);
    app.rgb888 = g_malloc0(app.rgb888_size);

    gtk_init(&argc, &argv);
    unlink(GUI_LOG_PATH);
    build_ui(&app);
    append_log(&app, "load driver first: ./scripts/load_driver.sh allow_dma_start=1");
    append_log(&app, "inference python: %s", infer_python());
    ensure_models_ready(&app, FALSE);
    gtk_widget_show_all(app.window);
    gtk_main();

    app.continuous = FALSE;
    if (app.connected && app.dev_fd >= 0) {
        ioctl(app.dev_fd, COLORBAR_IOC_SAFE_STOP);
        ioctl(app.dev_fd, COLORBAR_IOC_FREE_BUFS);
        close(app.dev_fd);
    }
    g_free(app.rgb565);
    g_free(app.rgb888);
    return 0;
}
