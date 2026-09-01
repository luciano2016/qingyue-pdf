#include "mupdf/fitz.h"
#include "mupdf/pdf.h"
#define NOMINMAX
#include <windows.h>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>

extern "C" void install_load_windows_font_funcs(fz_context* ctx);

namespace
{
    struct quiet_lock_table
    {
        CRITICAL_SECTION locks[FZ_LOCK_MAX];
        fz_locks_context context{};

        quiet_lock_table()
        {
            for (int i = 0; i < FZ_LOCK_MAX; ++i)
                InitializeCriticalSection(&locks[i]);
            context.user = this;
            context.lock = [](void* user, int lock)
            {
                EnterCriticalSection(&static_cast<quiet_lock_table*>(user)->locks[lock]);
            };
            context.unlock = [](void* user, int lock)
            {
                LeaveCriticalSection(&static_cast<quiet_lock_table*>(user)->locks[lock]);
            };
        }

        ~quiet_lock_table()
        {
            for (int i = 0; i < FZ_LOCK_MAX; ++i)
                DeleteCriticalSection(&locks[i]);
        }
    };

    quiet_lock_table& quiet_shared_locks()
    {
        static quiet_lock_table table;
        return table;
    }

    std::mutex& quiet_context_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    fz_context*& quiet_shared_root()
    {
        static fz_context* root{};
        return root;
    }
}

extern "C" fz_context* quiet_new_shared_context(size_t max_store)
{
    quiet_lock_table& table = quiet_shared_locks();
    std::lock_guard<std::mutex> guard(quiet_context_mutex());
    fz_context*& root = quiet_shared_root();
    if (!root)
    {
        size_t storeLimit = std::max<size_t>(max_store, 256ull * 1024ull * 1024ull);
        root = fz_new_context(nullptr, &table.context, storeLimit);
        if (root) install_load_windows_font_funcs(root);
    }
    return root ? fz_clone_context(root) : nullptr;
}

extern "C" fz_context* quiet_clone_context(fz_context* ctx)
{
    return ctx ? fz_clone_context(ctx) : nullptr;
}
extern "C" int quiet_shrink_store(fz_context* ctx, unsigned int percent)
{
    if (!ctx) return 0;
    return fz_shrink_store(ctx, std::min(percent, 100u));
}

extern "C" int quiet_shrink_shared_store(unsigned int percent)
{
    std::lock_guard<std::mutex> guard(quiet_context_mutex());
    fz_context* root = quiet_shared_root();
    return root ? fz_shrink_store(root, std::min(percent, 100u)) : 0;
}

static void quiet_set_last_error(const char* message);

namespace
{
    struct quiet_win32_stream
    {
        HANDLE file{ INVALID_HANDLE_VALUE };
        unsigned char buffer[4096]{};
    };

    int quiet_win32_stream_next(fz_context* ctx, fz_stream* stream, size_t)
    {
        auto* state = static_cast<quiet_win32_stream*>(stream->state);
        DWORD bytesRead = 0;
        if (!ReadFile(state->file, state->buffer, static_cast<DWORD>(sizeof(state->buffer)), &bytesRead, nullptr))
            fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot read shared PDF stream: Windows error %lu", GetLastError());
        stream->rp = state->buffer;
        stream->wp = state->buffer + bytesRead;
        stream->pos += static_cast<int64_t>(bytesRead);
        return bytesRead == 0 ? EOF : *stream->rp++;
    }

    void quiet_win32_stream_seek(fz_context* ctx, fz_stream* stream, int64_t offset, int whence)
    {
        auto* state = static_cast<quiet_win32_stream*>(stream->state);
        LARGE_INTEGER distance{};
        LARGE_INTEGER position{};
        distance.QuadPart = offset;
        DWORD method = whence == SEEK_SET ? FILE_BEGIN : (whence == SEEK_CUR ? FILE_CURRENT : FILE_END);
        if (!SetFilePointerEx(state->file, distance, &position, method))
            fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot seek shared PDF stream: Windows error %lu", GetLastError());
        stream->pos = position.QuadPart;
        stream->rp = state->buffer;
        stream->wp = state->buffer;
    }

    void quiet_win32_stream_drop(fz_context* ctx, void* opaque)
    {
        auto* state = static_cast<quiet_win32_stream*>(opaque);
        if (!state) return;
        if (state->file != INVALID_HANDLE_VALUE) CloseHandle(state->file);
        fz_free(ctx, state);
    }

    fz_stream* quiet_open_shared_stream(fz_context* ctx, const wchar_t* path)
    {
        HANDLE file = CreateFileW(
            path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
            fz_throw(ctx, FZ_ERROR_SYSTEM, "cannot open shared PDF stream: Windows error %lu", GetLastError());

        quiet_win32_stream* state = nullptr;
        fz_try(ctx)
        {
            state = fz_malloc_struct(ctx, quiet_win32_stream);
        }
        fz_catch(ctx)
        {
            CloseHandle(file);
            fz_rethrow(ctx);
        }
        state->file = file;
        fz_stream* stream = fz_new_stream(ctx, state, quiet_win32_stream_next, quiet_win32_stream_drop);
        stream->seek = quiet_win32_stream_seek;
        return stream;
    }
}

extern "C" fz_document* quiet_open_document_shared_w(fz_context* ctx, const wchar_t* path)
{
    if (!ctx || !path || !*path) return nullptr;
    fz_stream* stream = nullptr;
    fz_document* document = nullptr;
    fz_try(ctx)
    {
        stream = quiet_open_shared_stream(ctx, path);
        document = fz_open_document_with_stream(ctx, "application/pdf", stream);
    }
    fz_always(ctx)
    {
        fz_drop_stream(ctx, stream);
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        document = nullptr;
    }
    return document;
}
struct quiet_pdf_annotation
{
    int page;
    int kind;
    float x0;
    float y0;
    float x1;
    float y1;
    float color_r;
    float color_g;
    float color_b;
    float opacity;
    int64_t created_at_ms;
    const char* author;
    const char* contents;
};

struct quiet_pdf_annotation_info
{
    int page;
    int kind;
    float x0;
    float y0;
    float x1;
    float y1;
    float color_r;
    float color_g;
    float color_b;
    float opacity;
    int64_t created_at_ms;
    char author[128];
    char contents[512];
};

static char quiet_last_error[512] = "";

static void quiet_set_last_error(const char* message)
{
    if (!message) message = "";
    size_t i = 0;
    for (; i + 1 < sizeof(quiet_last_error) && message[i]; ++i)
        quiet_last_error[i] = message[i];
    quiet_last_error[i] = 0;
}

static void quiet_copy_text(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0)
        return;
    if (!src)
        src = "";
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}

extern "C" const char* quiet_pdf_last_error()
{
    return quiet_last_error;
}

extern "C" int quiet_pdf_get_page_bounds(
    fz_context* ctx,
    fz_document* fzdoc,
    fz_rect* out,
    int max_pages)
{
    quiet_set_last_error("");
    if (!ctx || !fzdoc || !out || max_pages <= 0)
    {
        quiet_set_last_error("invalid quiet_pdf_get_page_bounds arguments");
        return 0;
    }

    int count = 0;
    fz_try(ctx)
    {
        pdf_document* doc = pdf_specifics(ctx, fzdoc);
        if (!doc)
            fz_throw(ctx, FZ_ERROR_FORMAT, "document is not a PDF");

        count = std::min(pdf_count_pages(ctx, doc), max_pages);
        for (int page_index = 0; page_index < count; ++page_index)
        {
            fz_rect bounds = fz_make_rect(0, 0, 612, 792);
            fz_try(ctx)
            {
                pdf_obj* page_obj = pdf_lookup_page_obj(ctx, doc, page_index);
                fz_rect box{};
                fz_matrix transform{};
                pdf_page_obj_transform(ctx, page_obj, &box, &transform);
                fz_rect candidate = fz_transform_rect(box, transform);
                if (std::isfinite(candidate.x0) && std::isfinite(candidate.y0) &&
                    std::isfinite(candidate.x1) && std::isfinite(candidate.y1) &&
                    candidate.x1 - candidate.x0 >= 1 && candidate.y1 - candidate.y0 >= 1)
                    bounds = candidate;
            }
            fz_catch(ctx)
            {
                // Keep a standard page fallback for a malformed individual page.
            }
            out[page_index] = bounds;
        }
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        count = 0;
    }
    return count;
}

static bool quiet_page_has_supported_markup(fz_context* ctx, pdf_document* doc, int page_index)
{
    pdf_obj* page_obj = pdf_lookup_page_obj(ctx, doc, page_index);
    pdf_obj* annots = pdf_dict_get(ctx, page_obj, PDF_NAME(Annots));
    int annot_count = pdf_array_len(ctx, annots);
    for (int i = 0; i < annot_count; ++i)
    {
        pdf_obj* annot = pdf_array_get(ctx, annots, i);
        pdf_obj* subtype = pdf_dict_get(ctx, annot, PDF_NAME(Subtype));
        if (pdf_name_eq(ctx, subtype, PDF_NAME(Highlight)) ||
            pdf_name_eq(ctx, subtype, PDF_NAME(Underline)) ||
            pdf_name_eq(ctx, subtype, PDF_NAME(StrikeOut)))
            return true;
    }
    return false;
}

extern "C" int quietmupdf_anchor()
{
    return 0;
}
namespace
{
    thread_local int quiet_render_theme = 0;

    static void quiet_clear_theme_pixmap(fz_context* ctx, fz_pixmap* pix, int theme)
    {
        if (!pix) return;

        // The Win32 layer applies the document palette exactly once after
        // rendering. Keep MuPDF's destination white here so images, vectors,
        // and text all take the same final mapping path.
        (void)theme;
        fz_clear_pixmap_with_value(ctx, pix, 255);
    }

    static fz_device* quiet_wrap_theme_device(fz_context* ctx, fz_device* inner, int theme)
    {
        // Theme colors are applied once to the completed page bitmap by the
        // Win32 renderer, which also includes ordinary PDF images.
        (void)ctx;
        (void)theme;
        return inner;
    }
}

extern "C" void quiet_set_render_theme(int theme)
{
    quiet_render_theme = theme;
}

static fz_pixmap* quiet_new_pixmap_from_page_number_clip_impl(
    fz_context* ctx,
    fz_document* doc,
    int number,
    fz_matrix ctm,
    fz_colorspace* cs,
    int alpha,
    int x0,
    int y0,
    int x1,
    int y1,
    fz_cookie* cookie)
{
    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    fz_irect bbox{ x0, y0, x1, y1 };

    fz_try(ctx)
    {
        page = fz_load_page(ctx, doc, number);
        pix = fz_new_pixmap_with_bbox(ctx, cs, bbox, nullptr, alpha);
        quiet_clear_theme_pixmap(ctx, pix, quiet_render_theme);
        dev = quiet_wrap_theme_device(ctx, fz_new_draw_device(ctx, ctm, pix), quiet_render_theme);
        fz_run_page_contents(ctx, page, dev, fz_identity, cookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx)
    {
        if (dev) fz_drop_device(ctx, dev);
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx)
    {
        if (pix) fz_drop_pixmap(ctx, pix);
        pix = nullptr;
    }

    return pix;
}
extern "C" fz_display_list* quiet_new_page_contents_display_list_cookie(
    fz_context* ctx,
    fz_document* doc,
    int number,
    fz_cookie* cookie)
{
    fz_page* page = nullptr;
    fz_display_list* list = nullptr;
    fz_device* dev = nullptr;

    fz_try(ctx)
    {
        page = fz_load_page(ctx, doc, number);
        list = fz_new_display_list(ctx, fz_bound_page(ctx, page));
        dev = fz_new_list_device(ctx, list);
        fz_run_page_contents(ctx, page, dev, fz_identity, cookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx)
    {
        if (dev) fz_drop_device(ctx, dev);
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx)
    {
        if (list) fz_drop_display_list(ctx, list);
        list = nullptr;
    }

    if (cookie && cookie->abort && list)
    {
        fz_drop_display_list(ctx, list);
        list = nullptr;
    }
    return list;
}

extern "C" void quiet_drop_display_list(fz_context* ctx, fz_display_list* list)
{
    if (ctx && list) fz_drop_display_list(ctx, list);
}

extern "C" fz_pixmap* quiet_new_pixmap_from_display_list_clip_cookie(
    fz_context* ctx,
    fz_display_list* list,
    fz_matrix ctm,
    fz_colorspace* cs,
    int alpha,
    int x0,
    int y0,
    int x1,
    int y1,
    fz_cookie* cookie)
{
    if (!ctx || !list) return nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    fz_irect bbox{ x0, y0, x1, y1 };

    fz_try(ctx)
    {
        fz_rect scissor = fz_transform_rect(
            fz_make_rect(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1)),
            fz_invert_matrix(ctm));
        pix = fz_new_pixmap_with_bbox(ctx, cs, bbox, nullptr, alpha);
        quiet_clear_theme_pixmap(ctx, pix, quiet_render_theme);
        dev = quiet_wrap_theme_device(ctx, fz_new_draw_device(ctx, ctm, pix), quiet_render_theme);
        fz_run_display_list(ctx, list, dev, fz_identity, scissor, cookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx)
    {
        if (dev) fz_drop_device(ctx, dev);
    }
    fz_catch(ctx)
    {
        if (pix) fz_drop_pixmap(ctx, pix);
        pix = nullptr;
    }

    if (cookie && cookie->abort && pix)
    {
        fz_drop_pixmap(ctx, pix);
        pix = nullptr;
    }
    return pix;
}

extern "C" int quiet_render_display_list_clip_to_buffer(
    fz_context* ctx,
    fz_display_list* list,
    fz_matrix ctm,
    fz_colorspace* cs,
    int alpha,
    int x0,
    int y0,
    int x1,
    int y1,
    unsigned char* samples,
    int stride,
    fz_cookie* cookie)
{
    if (!ctx || !list || !samples || x1 <= x0 || y1 <= y0) return 0;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    int ok = 0;

    fz_try(ctx)
    {
        pix = fz_new_pixmap_with_data(ctx, cs, x1 - x0, y1 - y0, nullptr, alpha, stride, samples);
        pix->x = x0;
        pix->y = y0;
        quiet_clear_theme_pixmap(ctx, pix, quiet_render_theme);
        fz_rect scissor = fz_transform_rect(
            fz_make_rect(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1)),
            fz_invert_matrix(ctm));
        dev = quiet_wrap_theme_device(ctx, fz_new_draw_device(ctx, ctm, pix), quiet_render_theme);
        fz_run_display_list(ctx, list, dev, fz_identity, scissor, cookie);
        fz_close_device(ctx, dev);
        ok = !cookie || !cookie->abort;
    }
    fz_always(ctx)
    {
        if (dev) fz_drop_device(ctx, dev);
        if (pix) fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx)
    {
        ok = 0;
    }
    return ok;
}

extern "C" int quiet_render_page_number_clip_to_buffer(
    fz_context* ctx,
    fz_document* doc,
    int number,
    fz_matrix ctm,
    fz_colorspace* cs,
    int alpha,
    int x0,
    int y0,
    int x1,
    int y1,
    unsigned char* samples,
    int stride,
    fz_cookie* cookie)
{
    if (!ctx || !doc || !samples || x1 <= x0 || y1 <= y0) return 0;
    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    int ok = 0;

    fz_try(ctx)
    {
        page = fz_load_page(ctx, doc, number);
        pix = fz_new_pixmap_with_data(ctx, cs, x1 - x0, y1 - y0, nullptr, alpha, stride, samples);
        pix->x = x0;
        pix->y = y0;
        quiet_clear_theme_pixmap(ctx, pix, quiet_render_theme);
        dev = quiet_wrap_theme_device(ctx, fz_new_draw_device(ctx, ctm, pix), quiet_render_theme);
        fz_run_page_contents(ctx, page, dev, fz_identity, cookie);
        fz_close_device(ctx, dev);
        ok = !cookie || !cookie->abort;
    }
    fz_always(ctx)
    {
        if (dev) fz_drop_device(ctx, dev);
        if (pix) fz_drop_pixmap(ctx, pix);
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx)
    {
        ok = 0;
    }
    return ok;
}

extern "C" fz_cookie* quiet_new_render_cookie()
{
    return static_cast<fz_cookie*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(fz_cookie)));
}

extern "C" void quiet_abort_render_cookie(fz_cookie* cookie)
{
    if (cookie)
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&cookie->abort), 1);
}

extern "C" void quiet_drop_render_cookie(fz_cookie* cookie)
{
    if (cookie) HeapFree(GetProcessHeap(), 0, cookie);
}

extern "C" fz_pixmap* quiet_new_pixmap_from_page_number_clip(
    fz_context* ctx,
    fz_document* doc,
    int number,
    fz_matrix ctm,
    fz_colorspace* cs,
    int alpha,
    int x0,
    int y0,
    int x1,
    int y1)
{
    return quiet_new_pixmap_from_page_number_clip_impl(ctx, doc, number, ctm, cs, alpha, x0, y0, x1, y1, nullptr);
}

extern "C" fz_pixmap* quiet_new_pixmap_from_page_number_clip_cookie(
    fz_context* ctx,
    fz_document* doc,
    int number,
    fz_matrix ctm,
    fz_colorspace* cs,
    int alpha,
    int x0,
    int y0,
    int x1,
    int y1,
    fz_cookie* cookie)
{
    return quiet_new_pixmap_from_page_number_clip_impl(ctx, doc, number, ctm, cs, alpha, x0, y0, x1, y1, cookie);
}

static bool quiet_is_supported_markup(enum pdf_annot_type type)
{
    return type == PDF_ANNOT_HIGHLIGHT ||
        type == PDF_ANNOT_UNDERLINE ||
        type == PDF_ANNOT_STRIKE_OUT;
}

static bool quiet_is_user_comment_annot(enum pdf_annot_type type)
{
    switch (type)
    {
    case PDF_ANNOT_TEXT:
    case PDF_ANNOT_FREE_TEXT:
    case PDF_ANNOT_LINE:
    case PDF_ANNOT_SQUARE:
    case PDF_ANNOT_CIRCLE:
    case PDF_ANNOT_POLYGON:
    case PDF_ANNOT_POLY_LINE:
    case PDF_ANNOT_HIGHLIGHT:
    case PDF_ANNOT_UNDERLINE:
    case PDF_ANNOT_SQUIGGLY:
    case PDF_ANNOT_STRIKE_OUT:
    case PDF_ANNOT_REDACT:
    case PDF_ANNOT_STAMP:
    case PDF_ANNOT_CARET:
    case PDF_ANNOT_INK:
    case PDF_ANNOT_FILE_ATTACHMENT:
    case PDF_ANNOT_SOUND:
        return true;
    default:
        return false;
    }
}

static void quiet_set_rectangular_highlight_appearance(
    fz_context* ctx,
    pdf_document* doc,
    pdf_annot* annot,
    const quiet_pdf_annotation& input)
{
    fz_buffer* contents = nullptr;
    pdf_obj* resources = nullptr;
    fz_try(ctx)
    {
        pdf_obj* annotObj = pdf_annot_obj(ctx, annot);
        pdf_obj* points = pdf_dict_get(ctx, annotObj, PDF_NAME(QuadPoints));
        if (pdf_array_len(ctx, points) < 8)
            fz_throw(ctx, FZ_ERROR_FORMAT, "highlight annotation has no quad points");

        fz_rect rect = fz_empty_rect;
        for (int i = 0; i + 1 < pdf_array_len(ctx, points); i += 2)
        {
            float x = pdf_array_get_real(ctx, points, i);
            float y = pdf_array_get_real(ctx, points, i + 1);
            rect.x0 = fz_min(rect.x0, x);
            rect.y0 = fz_min(rect.y0, y);
            rect.x1 = fz_max(rect.x1, x);
            rect.y1 = fz_max(rect.y1, y);
        }
        if (fz_is_empty_rect(rect))
            fz_throw(ctx, FZ_ERROR_FORMAT, "highlight annotation has empty quad points");

        float opacity = fz_clamp(input.opacity, 0.12f, 1.0f);
        resources = pdf_new_dict(ctx, doc, 1);
        pdf_obj* states = pdf_dict_put_dict(ctx, resources, PDF_NAME(ExtGState), 1);
        pdf_obj* highlight = pdf_dict_put_dict(ctx, states, PDF_NAME(H), 4);
        pdf_dict_put(ctx, highlight, PDF_NAME(Type), PDF_NAME(ExtGState));
        pdf_dict_put(ctx, highlight, PDF_NAME(BM), PDF_NAME(Multiply));
        pdf_dict_put_real(ctx, highlight, PDF_NAME(CA), opacity);
        pdf_dict_put_real(ctx, highlight, PDF_NAME(ca), opacity);

        contents = fz_new_buffer(ctx, 96);
        fz_append_printf(
            ctx,
            contents,
            "q\n/H gs\n%g %g %g rg\n%g %g %g %g re\nf\nQ\n",
            input.color_r,
            input.color_g,
            input.color_b,
            rect.x0,
            rect.y0,
            rect.x1 - rect.x0,
            rect.y1 - rect.y0);

        pdf_dict_put_rect(ctx, annotObj, PDF_NAME(Rect), rect);
        pdf_set_annot_appearance(ctx, annot, "N", nullptr, fz_identity, rect, resources, contents);
    }
    fz_always(ctx)
    {
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);
    }
    fz_catch(ctx)
    {
        fz_rethrow(ctx);
    }
}
extern "C" int quiet_pdf_save_annotations(
    fz_context* ctx,
    fz_document* fzdoc,
    const char* filename,
    const quiet_pdf_annotation* annotations,
    int annotation_count)
{
    quiet_set_last_error("");
    if (!ctx || !fzdoc || !filename || annotation_count < 0 || (!annotations && annotation_count > 0))
    {
        quiet_set_last_error("invalid quiet_pdf_save_annotations arguments");
        return 0;
    }

    pdf_document* doc = nullptr;
    int ok = 0;

    fz_try(ctx)
    {
        doc = pdf_specifics(ctx, fzdoc);
        if (!doc)
            fz_throw(ctx, FZ_ERROR_FORMAT, "document is not a PDF");

        int page_count = pdf_count_pages(ctx, doc);
        for (int page_index = 0; page_index < page_count; ++page_index)
        {
            pdf_page* page = nullptr;
            fz_try(ctx)
            {
                page = pdf_load_page(ctx, doc, page_index);
                for (pdf_annot* annot = pdf_first_annot(ctx, page); annot; )
                {
                    pdf_annot* next = pdf_next_annot(ctx, annot);
                    enum pdf_annot_type type = pdf_annot_type(ctx, annot);
                    bool should_delete = annotation_count == 0 ?
                        quiet_is_user_comment_annot(type) :
                        quiet_is_supported_markup(type);
                    if (should_delete)
                        pdf_delete_annot(ctx, page, annot);
                    annot = next;
                }
            }
            fz_always(ctx)
            {
                if (page) pdf_drop_page(ctx, page);
            }
            fz_catch(ctx)
            {
                fz_rethrow(ctx);
            }
        }

        for (int i = 0; i < annotation_count; ++i)
        {
            const quiet_pdf_annotation& input = annotations[i];
            if (input.page < 0 || input.page >= page_count)
                continue;

            enum pdf_annot_type type = PDF_ANNOT_HIGHLIGHT;
            if (input.kind == 1) type = PDF_ANNOT_UNDERLINE;
            else if (input.kind == 2) type = PDF_ANNOT_STRIKE_OUT;

            pdf_page* page = nullptr;
            pdf_annot* annot = nullptr;
            fz_try(ctx)
            {
                page = pdf_load_page(ctx, doc, input.page);
                fz_rect bounds = pdf_bound_page(ctx, page, FZ_CROP_BOX);
                float left = bounds.x0 + fz_clamp(input.x0, 0.0f, 1.0f) * (bounds.x1 - bounds.x0);
                float top = bounds.y0 + fz_clamp(input.y0, 0.0f, 1.0f) * (bounds.y1 - bounds.y0);
                float right = bounds.x0 + fz_clamp(input.x1, 0.0f, 1.0f) * (bounds.x1 - bounds.x0);
                float bottom = bounds.y0 + fz_clamp(input.y1, 0.0f, 1.0f) * (bounds.y1 - bounds.y0);
                if (right < left) { float t = left; left = right; right = t; }
                if (bottom < top) { float t = top; top = bottom; bottom = t; }
                if (right - left >= 0.5f && bottom - top >= 0.5f)
                {
                    annot = pdf_create_annot(ctx, page, type);
                    fz_quad quad = fz_make_quad(left, top, right, top, left, bottom, right, bottom);
                    pdf_set_annot_quad_points(ctx, annot, 1, &quad);
                    int64_t date = input.created_at_ms > 0 ? input.created_at_ms / 1000 : static_cast<int64_t>(time(nullptr));
                    pdf_set_annot_creation_date(ctx, annot, date);
                    pdf_set_annot_modification_date(ctx, annot, date);
                    pdf_set_annot_author(ctx, annot, (input.author && input.author[0]) ? input.author : "10726");
                    if (input.contents && input.contents[0])
                        pdf_set_annot_contents(ctx, annot, input.contents);

                    if (type == PDF_ANNOT_HIGHLIGHT)
                    {
                        float color[3] = { input.color_r, input.color_g, input.color_b };
                        pdf_set_annot_color(ctx, annot, 3, color);
                        pdf_set_annot_opacity(ctx, annot, fz_clamp(input.opacity, 0.12f, 1.0f));
                    }
                    else if (type == PDF_ANNOT_UNDERLINE)
                    {
                        float color[3] = { input.color_r, input.color_g, input.color_b };
                        pdf_set_annot_color(ctx, annot, 3, color);
                    }
                    else
                    {
                        float color[3] = { input.color_r, input.color_g, input.color_b };
                        pdf_set_annot_color(ctx, annot, 3, color);
                    }

                    pdf_update_annot(ctx, annot);
                    if (type == PDF_ANNOT_HIGHLIGHT)
                        quiet_set_rectangular_highlight_appearance(ctx, doc, annot, input);
                }
            }
            fz_always(ctx)
            {
                if (annot) pdf_drop_annot(ctx, annot);
                if (page) pdf_drop_page(ctx, page);
            }
            fz_catch(ctx)
            {
                fz_rethrow(ctx);
            }
        }

        pdf_write_options opts = pdf_default_write_options;
        opts.do_incremental = 0;
        opts.do_compress = 1;
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        opts.do_garbage = 0;
        opts.do_appearance = 1;
        pdf_save_document(ctx, doc, filename, &opts);
        ok = 1;
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        ok = 0;
    }

    return ok;
}

extern "C" int quiet_pdf_load_annotations(
    fz_context* ctx,
    fz_document* fzdoc,
    quiet_pdf_annotation_info* out,
    int max_annotations)
{
    quiet_set_last_error("");
    if (!ctx || !fzdoc || !out || max_annotations <= 0)
    {
        quiet_set_last_error("invalid quiet_pdf_load_annotations arguments");
        return 0;
    }

    pdf_document* doc = nullptr;
    int count = 0;

    fz_try(ctx)
    {
        doc = pdf_specifics(ctx, fzdoc);
        if (!doc)
            fz_throw(ctx, FZ_ERROR_FORMAT, "document is not a PDF");

        int page_count = pdf_count_pages(ctx, doc);
        for (int page_index = 0; page_index < page_count && count < max_annotations; ++page_index)
        {
            if (!quiet_page_has_supported_markup(ctx, doc, page_index))
                continue;
            pdf_page* page = nullptr;
            fz_try(ctx)
            {
                page = pdf_load_page(ctx, doc, page_index);
                fz_rect bounds = pdf_bound_page(ctx, page, FZ_CROP_BOX);
                float page_w = bounds.x1 - bounds.x0;
                float page_h = bounds.y1 - bounds.y0;
                if (page_w > 0 && page_h > 0)
                {
                    for (pdf_annot* annot = pdf_first_annot(ctx, page); annot && count < max_annotations; annot = pdf_next_annot(ctx, annot))
                    {
                        enum pdf_annot_type type = pdf_annot_type(ctx, annot);
                        int kind = -1;
                        if (type == PDF_ANNOT_HIGHLIGHT) kind = 0;
                        else if (type == PDF_ANNOT_UNDERLINE) kind = 1;
                        else if (type == PDF_ANNOT_STRIKE_OUT) kind = 2;
                        if (kind < 0)
                            continue;

                        float color[4] = { 1.0f, 0.82f, 0.0f, 1.0f };
                        int n = 0;
                        pdf_annot_color(ctx, annot, &n, color);
                        int64_t date = pdf_annot_creation_date(ctx, annot);
                        if (date <= 0) date = pdf_annot_modification_date(ctx, annot);
                        const char* author = pdf_annot_author(ctx, annot);
                        const char* contents = pdf_annot_contents(ctx, annot);

                        auto append_rect = [&](fz_rect rect)
                        {
                            if (count >= max_annotations || rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                                return;
                            quiet_pdf_annotation_info& item = out[count];
                            std::memset(&item, 0, sizeof(item));
                            item.page = page_index;
                            item.kind = kind;
                            item.x0 = fz_clamp((rect.x0 - bounds.x0) / page_w, 0.0f, 1.0f);
                            item.y0 = fz_clamp((rect.y0 - bounds.y0) / page_h, 0.0f, 1.0f);
                            item.x1 = fz_clamp((rect.x1 - bounds.x0) / page_w, 0.0f, 1.0f);
                            item.y1 = fz_clamp((rect.y1 - bounds.y0) / page_h, 0.0f, 1.0f);
                            item.color_r = n > 0 ? color[0] : 1.0f;
                            item.color_g = n > 1 ? color[1] : 0.82f;
                            item.color_b = n > 2 ? color[2] : 0.0f;
                            item.opacity = pdf_annot_opacity(ctx, annot);
                            item.created_at_ms = date > 0 ? date * 1000 : 0;
                            quiet_copy_text(item.author, sizeof(item.author), author);
                            quiet_copy_text(item.contents, sizeof(item.contents), contents);
                            ++count;
                        };

                        int quad_count = pdf_annot_quad_point_count(ctx, annot);
                        if (quad_count > 0)
                        {
                            for (int q = 0; q < quad_count && count < max_annotations; ++q)
                                append_rect(fz_rect_from_quad(pdf_annot_quad_point(ctx, annot, q)));
                        }
                        else
                        {
                            append_rect(pdf_annot_rect(ctx, annot));
                        }
                    }
                }
            }
            fz_always(ctx)
            {
                if (page) pdf_drop_page(ctx, page);
            }
            fz_catch(ctx)
            {
                fz_rethrow(ctx);
            }
        }
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        count = 0;
    }

    return count;
}

static bool quiet_is_whitespace(int c)
{
    return c <= 0x20 || c == 0x3000;
}

static bool quiet_is_ascii_word(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
}

static bool quiet_is_cjk(int c)
{
    return (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x2EBEF);
}

static bool quiet_is_cjk_punctuation(int c)
{
    switch (c)
    {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF01: case 0xFF1F: case 0xFF1B: case 0xFF1A:
    case 0x201C: case 0x201D: case 0x2018: case 0x2019: case 0xFF08: case 0xFF09: case 0x300A:
    case 0x300B: case 0x3010: case 0x3011: case 0x300C: case 0x300D: case 0x300E: case 0x300F:
    case 0x2026: case 0x2014: case 0x00B7:
        return true;
    default:
        return false;
    }
}

static bool quiet_is_ascii_punctuation(int c)
{
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

static int quiet_token_class(int c)
{
    if (quiet_is_whitespace(c) || quiet_is_ascii_punctuation(c) || quiet_is_cjk_punctuation(c))
        return 0;
    if (quiet_is_ascii_word(c))
        return 1;
    if (quiet_is_cjk(c))
        return 2;
    return 3;
}

static bool quiet_is_non_cjk_search_word_char(int c)
{
    return quiet_token_class(c) == 1;
}

static fz_rect quiet_rect_from_quad(fz_quad q)
{
    fz_rect r;
    r.x0 = std::min({ q.ul.x, q.ur.x, q.ll.x, q.lr.x });
    r.y0 = std::min({ q.ul.y, q.ur.y, q.ll.y, q.lr.y });
    r.x1 = std::max({ q.ul.x, q.ur.x, q.ll.x, q.lr.x });
    r.y1 = std::max({ q.ul.y, q.ur.y, q.ll.y, q.lr.y });
    return r;
}

static bool quiet_rects_intersect(fz_rect a, fz_rect b)
{
    return a.x0 < b.x1 && a.x1 > b.x0 && a.y0 < b.y1 && a.y1 > b.y0;
}

static bool quiet_search_hit_has_word_boundaries(
    fz_stext_page* stext,
    const fz_quad* quads,
    int quad_count)
{
    if (!stext || !quads || quad_count <= 0)
        return true;

    struct search_char
    {
        int c;
        fz_rect rect;
    };
    std::vector<search_char> chars;
    for (fz_stext_block* block = stext->first_block; block; block = block->next)
    {
        if (block->type != FZ_STEXT_BLOCK_TEXT)
            continue;
        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next)
        {
            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next)
            {
                fz_rect rect = quiet_rect_from_quad(ch->quad);
                if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                    continue;
                chars.push_back(search_char{ ch->c, rect });
            }
        }
    }

    if (chars.empty())
        return true;

    int first = -1;
    int last = -1;
    for (int i = 0; i < static_cast<int>(chars.size()); ++i)
    {
        for (int q = 0; q < quad_count; ++q)
        {
            fz_rect hit = quiet_rect_from_quad(quads[q]);
            if (!quiet_rects_intersect(chars[static_cast<size_t>(i)].rect, hit))
                continue;
            if (first < 0)
                first = i;
            last = i;
            break;
        }
    }

    if (first < 0 || last < first)
        return true;

    const int firstChar = chars[static_cast<size_t>(first)].c;
    const int lastChar = chars[static_cast<size_t>(last)].c;
    bool leftOk = !quiet_is_non_cjk_search_word_char(firstChar) ||
        first == 0 ||
        !quiet_is_non_cjk_search_word_char(chars[static_cast<size_t>(first - 1)].c);
    bool rightOk = !quiet_is_non_cjk_search_word_char(lastChar) ||
        last + 1 >= static_cast<int>(chars.size()) ||
        !quiet_is_non_cjk_search_word_char(chars[static_cast<size_t>(last + 1)].c);
    return leftOk && rightOk;
}

extern "C" int quiet_pdf_search_page_number(
    fz_context* ctx,
    fz_document* doc,
    int page_number,
    const char* needle,
    int* hit_mark,
    fz_quad* hit_bbox,
    int hit_max,
    int search_options,
    int whole_word)
{
    quiet_set_last_error("");
    if (!ctx || !doc || page_number < 0 || !needle || !hit_bbox || hit_max <= 0)
    {
        quiet_set_last_error("invalid quiet_pdf_search_page_number arguments");
        return 0;
    }

    fz_page* page = nullptr;
    fz_stext_page* stext = nullptr;
    std::vector<int> marks(static_cast<size_t>(hit_max));
    std::vector<fz_quad> boxes(static_cast<size_t>(hit_max));
    int out_count = 0;

    fz_try(ctx)
    {
        page = fz_load_page(ctx, doc, page_number);
        fz_stext_options opts{};
        opts.flags = FZ_STEXT_DEHYPHENATE;
        stext = fz_new_stext_page_from_page(ctx, page, &opts);
        int count = fz_match_stext_page(ctx, stext, needle, marks.data(), boxes.data(), hit_max, static_cast<fz_search_options>(search_options));

        for (int i = 0; i < count && i < hit_max;)
        {
            int mark = marks[static_cast<size_t>(i)];
            int group_end = i + 1;
            while (group_end < count && group_end < hit_max && marks[static_cast<size_t>(group_end)] == mark)
                ++group_end;

            bool keep = !whole_word ||
                quiet_search_hit_has_word_boundaries(stext, boxes.data() + i, group_end - i);
            if (keep)
            {
                for (int j = i; j < group_end && out_count < hit_max; ++j)
                {
                    if (hit_mark)
                        hit_mark[out_count] = marks[static_cast<size_t>(j)];
                    hit_bbox[out_count] = boxes[static_cast<size_t>(j)];
                    ++out_count;
                }
            }
            i = group_end;
        }
    }
    fz_always(ctx)
    {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        out_count = 0;
    }

    return out_count;
}

static fz_rect quiet_inflate_rect(fz_rect r, float pad)
{
    r.x0 -= pad;
    r.y0 -= pad;
    r.x1 += pad;
    r.y1 += pad;
    return r;
}

static bool quiet_point_in_rect(fz_point p, fz_rect r)
{
    return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1;
}

static float quiet_point_rect_distance_sq(fz_point p, fz_rect r)
{
    float dx = 0.0f;
    if (p.x < r.x0) dx = r.x0 - p.x;
    else if (p.x > r.x1) dx = p.x - r.x1;
    float dy = 0.0f;
    if (p.y < r.y0) dy = r.y0 - p.y;
    else if (p.y > r.y1) dy = p.y - r.y1;
    return dx * dx + dy * dy;
}

static void quiet_normalize_quad(fz_quad& q, fz_rect bounds)
{
    float width = std::max(0.0001f, bounds.x1 - bounds.x0);
    float height = std::max(0.0001f, bounds.y1 - bounds.y0);
    q.ul.x = (q.ul.x - bounds.x0) / width;
    q.ur.x = (q.ur.x - bounds.x0) / width;
    q.ll.x = (q.ll.x - bounds.x0) / width;
    q.lr.x = (q.lr.x - bounds.x0) / width;
    q.ul.y = (q.ul.y - bounds.y0) / height;
    q.ur.y = (q.ur.y - bounds.y0) / height;
    q.ll.y = (q.ll.y - bounds.y0) / height;
    q.lr.y = (q.lr.y - bounds.y0) / height;
}

extern "C" int quiet_pdf_get_text_char_boxes(
    fz_context* ctx,
    fz_document* doc,
    int page_number,
    fz_rect* boxes,
    int max_boxes)
{
    quiet_set_last_error("");
    if (!ctx || !doc || page_number < 0 || max_boxes < 0 || (max_boxes > 0 && !boxes))
    {
        quiet_set_last_error("invalid quiet_pdf_get_text_char_boxes arguments");
        return 0;
    }

    fz_stext_page* stext = nullptr;
    int count = 0;
    fz_try(ctx)
    {
        int page_count = fz_count_pages(ctx, doc);
        if (page_number >= page_count)
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "page out of range");

        fz_page* page = fz_load_page(ctx, doc, page_number);
        fz_rect bounds = fz_bound_page(ctx, page);
        fz_drop_page(ctx, page);

        fz_stext_options opts{};
        opts.flags = FZ_STEXT_DEHYPHENATE;
        stext = fz_new_stext_page_from_page_number(ctx, doc, page_number, &opts);

        float width = std::max(0.0001f, bounds.x1 - bounds.x0);
        float height = std::max(0.0001f, bounds.y1 - bounds.y0);
        for (fz_stext_block* block = stext ? stext->first_block : nullptr; block; block = block->next)
        {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next)
            {
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next)
                {
                    if (ch->c == 0 || ch->c == '\r' || ch->c == '\n')
                        continue;
                    fz_rect rect = quiet_rect_from_quad(ch->quad);
                    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                        continue;
                    if (boxes && count < max_boxes)
                    {
                        boxes[count] = fz_rect{
                            (rect.x0 - bounds.x0) / width,
                            (rect.y0 - bounds.y0) / height,
                            (rect.x1 - bounds.x0) / width,
                            (rect.y1 - bounds.y0) / height
                        };
                    }
                    ++count;
                }
            }
        }
    }
    fz_always(ctx)
    {
        if (stext) fz_drop_stext_page(ctx, stext);
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        count = 0;
    }

    return count;
}
extern "C" int quiet_pdf_select_word_at_point(
    fz_context* ctx,
    fz_document* doc,
    int page_number,
    float x,
    float y,
    fz_quad* quads,
    int max_quads,
    char* text,
    int text_capacity)
{
    quiet_set_last_error("");
    if (!ctx || !doc || page_number < 0 || !quads || max_quads <= 0)
    {
        quiet_set_last_error("invalid quiet_pdf_select_word_at_point arguments");
        return 0;
    }

    if (text && text_capacity > 0)
        text[0] = 0;

    fz_stext_page* stext = nullptr;
    fz_buffer* text_buffer = nullptr;
    int count = 0;

    fz_try(ctx)
    {
        int page_count = fz_count_pages(ctx, doc);
        if (page_number >= page_count)
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "page out of range");

        fz_page* page = fz_load_page(ctx, doc, page_number);
        fz_rect bounds = fz_bound_page(ctx, page);
        fz_drop_page(ctx, page);

        fz_stext_options opts{};
        opts.flags = FZ_STEXT_DEHYPHENATE;
        stext = fz_new_stext_page_from_page_number(ctx, doc, page_number, &opts);

        fz_point point{
            bounds.x0 + fz_clamp(x, 0.0f, 1.0f) * (bounds.x1 - bounds.x0),
            bounds.y0 + fz_clamp(y, 0.0f, 1.0f) * (bounds.y1 - bounds.y0)
        };

        fz_stext_line* best_line = nullptr;
        int best_index = -1;
        float best_score = 3.402823466e+38F;

        for (fz_stext_block* block = stext ? stext->first_block : nullptr; block; block = block->next)
        {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next)
            {
                int index = 0;
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next, ++index)
                {
                    fz_rect rect = quiet_rect_from_quad(ch->quad);
                    float pad = std::max(1.5f, ch->size * 0.28f);
                    if (!quiet_point_in_rect(point, quiet_inflate_rect(rect, pad)))
                        continue;
                    float score = quiet_point_rect_distance_sq(point, rect);
                    if (score < best_score)
                    {
                        best_score = score;
                        best_line = line;
                        best_index = index;
                    }
                }
            }
        }

        if (best_line && best_index >= 0)
        {
            std::vector<fz_stext_char*> chars;
            for (fz_stext_char* ch = best_line->first_char; ch; ch = ch->next)
                chars.push_back(ch);

            if (best_index < static_cast<int>(chars.size()))
            {
                int cls = quiet_token_class(chars[static_cast<size_t>(best_index)]->c);
                if (cls != 0)
                {
                    auto compatible = [cls](int c) -> bool
                    {
                        int other = quiet_token_class(c);
                        if (cls == 1) return other == 1;
                        if (cls == 2) return other == 2;
                        return other == cls;
                    };

                    int start = best_index;
                    int end = best_index;
                    while (start > 0 && compatible(chars[static_cast<size_t>(start - 1)]->c))
                        --start;
                    while (end + 1 < static_cast<int>(chars.size()) && compatible(chars[static_cast<size_t>(end + 1)]->c))
                        ++end;

                    text_buffer = fz_new_buffer(ctx, 256);
                    fz_rect merged{};
                    bool have_merged = false;
                    for (int i = start; i <= end; ++i)
                    {
                        fz_stext_char* ch = chars[static_cast<size_t>(i)];
                        fz_rect char_rect = quiet_rect_from_quad(ch->quad);
                        if (!have_merged)
                        {
                            merged = char_rect;
                            have_merged = true;
                        }
                        else
                        {
                            merged.x0 = std::min(merged.x0, char_rect.x0);
                            merged.y0 = std::min(merged.y0, char_rect.y0);
                            merged.x1 = std::max(merged.x1, char_rect.x1);
                            merged.y1 = std::max(merged.y1, char_rect.y1);
                        }
                        if (text_buffer)
                            fz_append_rune(ctx, text_buffer, ch->c);
                    }
                    if (have_merged && count < max_quads)
                    {
                        fz_quad quad{};
                        quad.ul = fz_point{ merged.x0, merged.y0 };
                        quad.ur = fz_point{ merged.x1, merged.y0 };
                        quad.ll = fz_point{ merged.x0, merged.y1 };
                        quad.lr = fz_point{ merged.x1, merged.y1 };
                        quiet_normalize_quad(quad, bounds);
                        quads[count++] = quad;
                    }

                    if (text && text_capacity > 0 && text_buffer)
                    {
                        fz_terminate_buffer(ctx, text_buffer);
                        quiet_copy_text(text, static_cast<size_t>(text_capacity), fz_string_from_buffer(ctx, text_buffer));
                    }
                }
            }
        }
    }
    fz_always(ctx)
    {
        if (text_buffer) fz_drop_buffer(ctx, text_buffer);
        if (stext) fz_drop_stext_page(ctx, stext);
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        count = 0;
        if (text && text_capacity > 0)
            text[0] = 0;
    }

    return count;
}
struct quiet_visual_char
{
    fz_stext_char* ch;
    fz_rect rect;
    float cx;
    float cy;
    float size;
};

struct quiet_visual_line
{
    std::vector<quiet_visual_char> chars;
    float y0;
    float y1;
    float cy;
    float size;
};

struct quiet_pdf_link_info
{
    float x0;
    float y0;
    float x1;
    float y1;
    char uri[1024];
};

struct quiet_visual_caret
{
    int line;
    int boundary;
};

enum class quiet_gap_policy
{
    nearest,
    previous_line_end,
    next_line_start
};

static int quiet_clamp_int(int value, int low, int high)
{
    return std::min(std::max(value, low), high);
}
static bool quiet_is_selectable_rune(int c)
{
    return c != 0 && c != '\r' && c != '\n';
}

static bool quiet_line_accepts_char(const quiet_visual_line& line, const quiet_visual_char& ch)
{
    // Glyph rectangles may overlap vertically across adjacent code lines. Group by
    // baseline center only, so a rectangle overlap cannot turn two text rows into
    // one selectable visual line.
    float reference_size = std::max(1.0f, std::min(line.size, ch.size));
    float tolerance = std::max(1.2f, reference_size * 0.30f);
    return std::abs(ch.cy - line.cy) <= tolerance;
}

static void quiet_add_char_to_line(quiet_visual_line& line, const quiet_visual_char& ch)
{
    line.chars.push_back(ch);
    line.y0 = std::min(line.y0, ch.rect.y0);
    line.y1 = std::max(line.y1, ch.rect.y1);
    line.cy = (line.y0 + line.y1) * 0.5f;
    line.size = std::max(line.size, ch.size);
}

static std::vector<quiet_visual_line> quiet_build_visual_lines(fz_stext_page* stext)
{
    std::vector<quiet_visual_char> chars;
    for (fz_stext_block* block = stext ? stext->first_block : nullptr; block; block = block->next)
    {
        if (block->type != FZ_STEXT_BLOCK_TEXT)
            continue;
        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next)
        {
            for (fz_stext_char* ch = line->first_char; ch; ch = ch->next)
            {
                if (!quiet_is_selectable_rune(ch->c))
                    continue;
                fz_rect rect = quiet_rect_from_quad(ch->quad);
                if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                    continue;
                chars.push_back(quiet_visual_char{
                    ch,
                    rect,
                    (rect.x0 + rect.x1) * 0.5f,
                    (rect.y0 + rect.y1) * 0.5f,
                    std::max(1.0f, ch->size)
                });
            }
        }
    }

    std::sort(chars.begin(), chars.end(), [](const quiet_visual_char& a, const quiet_visual_char& b)
    {
        float ay = a.cy;
        float by = b.cy;
        float tolerance = std::max({ a.size, b.size, 1.0f }) * 0.45f;
        if (std::abs(ay - by) > tolerance)
            return ay < by;
        return a.cx < b.cx;
    });

    std::vector<quiet_visual_line> lines;
    for (const quiet_visual_char& ch : chars)
    {
        int best = -1;
        float bestDistance = 3.402823466e+38F;
        for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i)
        {
            if (ch.cy > lines[static_cast<size_t>(i)].cy + std::max(8.0f, ch.size * 2.0f))
                break;
            if (!quiet_line_accepts_char(lines[static_cast<size_t>(i)], ch))
                continue;
            float distance = std::abs(ch.cy - lines[static_cast<size_t>(i)].cy);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }
        if (best >= 0)
        {
            quiet_add_char_to_line(lines[static_cast<size_t>(best)], ch);
        }
        else
        {
            quiet_visual_line line;
            line.y0 = ch.rect.y0;
            line.y1 = ch.rect.y1;
            line.cy = ch.cy;
            line.size = ch.size;
            line.chars.push_back(ch);
            lines.push_back(std::move(line));
        }
    }

    for (quiet_visual_line& line : lines)
    {
        std::sort(line.chars.begin(), line.chars.end(), [](const quiet_visual_char& a, const quiet_visual_char& b)
        {
            return a.cx < b.cx;
        });
    }
    std::sort(lines.begin(), lines.end(), [](const quiet_visual_line& a, const quiet_visual_line& b)
    {
        if (std::abs(a.cy - b.cy) > 1.0f)
            return a.cy < b.cy;
        float ax = a.chars.empty() ? 0.0f : a.chars.front().cx;
        float bx = b.chars.empty() ? 0.0f : b.chars.front().cx;
        return ax < bx;
    });
    return lines;
}

struct quiet_link_record
{
    fz_rect rect;
    std::string uri;
};

static char quiet_ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

static bool quiet_ascii_starts_with(const char* text, const char* prefix)
{
    if (!text || !prefix) return false;
    while (*prefix)
    {
        if (!*text || quiet_ascii_lower(*text) != quiet_ascii_lower(*prefix))
            return false;
        ++text;
        ++prefix;
    }
    return true;
}

static bool quiet_is_supported_external_uri(const char* uri)
{
    return quiet_ascii_starts_with(uri, "http://") ||
        quiet_ascii_starts_with(uri, "https://") ||
        quiet_ascii_starts_with(uri, "mailto:") ||
        quiet_ascii_starts_with(uri, "ftp://") ||
        quiet_ascii_starts_with(uri, "www.");
}

static fz_rect quiet_normalized_rect(fz_rect rect, fz_rect bounds)
{
    float width = std::max(0.0001f, bounds.x1 - bounds.x0);
    float height = std::max(0.0001f, bounds.y1 - bounds.y0);
    return fz_rect{
        (rect.x0 - bounds.x0) / width,
        (rect.y0 - bounds.y0) / height,
        (rect.x1 - bounds.x0) / width,
        (rect.y1 - bounds.y0) / height
    };
}

static bool quiet_link_rect_overlaps(const std::vector<quiet_link_record>& links, fz_rect candidate)
{
    float candidateArea = std::max(0.0f, candidate.x1 - candidate.x0) *
        std::max(0.0f, candidate.y1 - candidate.y0);
    if (candidateArea <= 0.0f) return true;
    for (const quiet_link_record& link : links)
    {
        float x0 = std::max(link.rect.x0, candidate.x0);
        float y0 = std::max(link.rect.y0, candidate.y0);
        float x1 = std::min(link.rect.x1, candidate.x1);
        float y1 = std::min(link.rect.y1, candidate.y1);
        float overlap = std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0);
        if (overlap / candidateArea >= 0.25f)
            return true;
    }
    return false;
}

static bool quiet_is_url_prefix(const std::string& text, size_t index)
{
    const char* value = text.c_str() + index;
    return quiet_ascii_starts_with(value, "http://") ||
        quiet_ascii_starts_with(value, "https://") ||
        quiet_ascii_starts_with(value, "www.");
}

static bool quiet_is_url_boundary(char value)
{
    return value == 0 || static_cast<unsigned char>(value) <= 32 ||
        value == '<' || value == '>' || value == '"';
}

static bool quiet_is_trailing_url_punctuation(char value)
{
    return value == '.' || value == ',' || value == ';' || value == ':' ||
        value == '!' || value == '?' || value == ')' || value == ']' || value == '}';
}

extern "C" int quiet_pdf_get_links(
    fz_context* ctx,
    fz_document* doc,
    int page_number,
    quiet_pdf_link_info* output,
    int max_links,
    fz_rect* text_boxes,
    int max_text_boxes,
    int* text_box_count)
{
    quiet_set_last_error("");
    if (!ctx || !doc || page_number < 0 || max_links < 0 || (max_links > 0 && !output) ||
        max_text_boxes < 0 || (max_text_boxes > 0 && !text_boxes) || !text_box_count)
    {
        quiet_set_last_error("invalid quiet_pdf_get_links arguments");
        return 0;
    }
    *text_box_count = 0;

    fz_page* page = nullptr;
    fz_link* pageLinks = nullptr;
    fz_stext_page* stext = nullptr;
    std::vector<quiet_link_record> links;
    int resultCount = 0;
    int characterCount = 0;
    fz_try(ctx)
    {
        if (page_number >= fz_count_pages(ctx, doc))
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "page out of range");

        page = fz_load_page(ctx, doc, page_number);
        fz_rect bounds = fz_bound_page(ctx, page);
        pageLinks = fz_load_links(ctx, page);
        for (fz_link* link = pageLinks; link; link = link->next)
        {
            if (!quiet_is_supported_external_uri(link->uri))
                continue;
            std::string uri = link->uri;
            if (quiet_ascii_starts_with(uri.c_str(), "www."))
                uri.insert(0, "https://");
            links.push_back(quiet_link_record{ quiet_normalized_rect(link->rect, bounds), std::move(uri) });
        }

        fz_stext_options options{};
        options.flags = FZ_STEXT_DEHYPHENATE;
        stext = fz_new_stext_page_from_page_number(ctx, doc, page_number, &options);
        std::vector<quiet_visual_line> lines = quiet_build_visual_lines(stext);
        for (const quiet_visual_line& line : lines)
        {
            for (const quiet_visual_char& ch : line.chars)
            {
                if (text_boxes && characterCount < max_text_boxes)
                    text_boxes[characterCount] = quiet_normalized_rect(ch.rect, bounds);
                ++characterCount;
            }
        }
        *text_box_count = characterCount;
        for (const quiet_visual_line& line : lines)
        {
            std::string text;
            text.reserve(line.chars.size());
            for (const quiet_visual_char& ch : line.chars)
                text.push_back(ch.ch->c > 0 && ch.ch->c < 128 ? static_cast<char>(ch.ch->c) : ' ');

            for (size_t start = 0; start < text.size();)
            {
                bool prefixBoundary = start == 0 || quiet_is_url_boundary(text[start - 1]) ||
                    text[start - 1] == '(' || text[start - 1] == '[';
                if (!prefixBoundary || !quiet_is_url_prefix(text, start))
                {
                    ++start;
                    continue;
                }

                size_t end = start;
                while (end < text.size() && !quiet_is_url_boundary(text[end]))
                    ++end;
                while (end > start && quiet_is_trailing_url_punctuation(text[end - 1]))
                    --end;
                if (end <= start + 4)
                {
                    ++start;
                    continue;
                }

                fz_rect rect = line.chars[start].rect;
                for (size_t i = start + 1; i < end; ++i)
                {
                    rect.x0 = std::min(rect.x0, line.chars[i].rect.x0);
                    rect.y0 = std::min(rect.y0, line.chars[i].rect.y0);
                    rect.x1 = std::max(rect.x1, line.chars[i].rect.x1);
                    rect.y1 = std::max(rect.y1, line.chars[i].rect.y1);
                }
                fz_rect normalized = quiet_normalized_rect(rect, bounds);
                if (!quiet_link_rect_overlaps(links, normalized))
                {
                    std::string uri = text.substr(start, end - start);
                    if (quiet_ascii_starts_with(uri.c_str(), "www."))
                        uri.insert(0, "https://");
                    links.push_back(quiet_link_record{ normalized, std::move(uri) });
                }
                start = std::max(end, start + 1);
            }
        }

        resultCount = static_cast<int>(links.size());
        for (int i = 0; i < std::min(resultCount, max_links); ++i)
        {
            output[i].x0 = links[static_cast<size_t>(i)].rect.x0;
            output[i].y0 = links[static_cast<size_t>(i)].rect.y0;
            output[i].x1 = links[static_cast<size_t>(i)].rect.x1;
            output[i].y1 = links[static_cast<size_t>(i)].rect.y1;
            strncpy_s(
                output[i].uri,
                sizeof(output[i].uri),
                links[static_cast<size_t>(i)].uri.c_str(),
                _TRUNCATE);
        }
    }
    fz_always(ctx)
    {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (pageLinks) fz_drop_link(ctx, pageLinks);
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        resultCount = 0;
        *text_box_count = 0;
    }
    return resultCount;
}

static int quiet_char_boundary_index(const quiet_visual_line& line, fz_point p)
{
    if (line.chars.empty()) return 0;
    int best = 0;
    float bestDistance = std::abs(p.x - line.chars.front().rect.x0);
    for (int i = 0; i < static_cast<int>(line.chars.size()); ++i)
    {
        const quiet_visual_char& ch = line.chars[static_cast<size_t>(i)];
        float leftDistance = std::abs(p.x - ch.rect.x0);
        if (leftDistance < bestDistance)
        {
            bestDistance = leftDistance;
            best = i;
        }
        float rightDistance = std::abs(p.x - ch.rect.x1);
        if (rightDistance < bestDistance)
        {
            bestDistance = rightDistance;
            best = i + 1;
        }
        if (p.x >= ch.rect.x0 && p.x <= ch.rect.x1)
            return (p.x < ch.cx) ? i : i + 1;
    }
    return quiet_clamp_int(best, 0, static_cast<int>(line.chars.size()));
}

static quiet_visual_caret quiet_caret_for_visual_point(
    const std::vector<quiet_visual_line>& lines,
    fz_point p,
    quiet_gap_policy gap_policy)
{
    if (lines.empty())
        return quiet_visual_caret{ -1, 0 };

    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
    {
        const quiet_visual_line& line = lines[static_cast<size_t>(i)];
        if (line.chars.empty())
            continue;

        float pad = std::max(1.0f, line.size * 0.16f);
        if (p.y >= line.y0 - pad && p.y <= line.y1 + pad)
            return quiet_visual_caret{ i, quiet_char_boundary_index(line, p) };

        if (p.y < line.y0 - pad)
        {
            if (i == 0)
                return quiet_visual_caret{ i, 0 };

            int prev_index = i - 1;
            const quiet_visual_line& prev = lines[static_cast<size_t>(prev_index)];
            if (gap_policy == quiet_gap_policy::previous_line_end)
                return quiet_visual_caret{ prev_index, static_cast<int>(prev.chars.size()) };
            if (gap_policy == quiet_gap_policy::next_line_start)
                return quiet_visual_caret{ i, 0 };

            float prev_distance = std::abs(p.y - prev.y1);
            float next_distance = std::abs(line.y0 - p.y);
            if (prev_distance <= next_distance)
                return quiet_visual_caret{ prev_index, quiet_char_boundary_index(prev, p) };
            return quiet_visual_caret{ i, quiet_char_boundary_index(line, p) };
        }
    }

    for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i)
    {
        const quiet_visual_line& line = lines[static_cast<size_t>(i)];
        if (!line.chars.empty())
            return quiet_visual_caret{ i, static_cast<int>(line.chars.size()) };
    }
    return quiet_visual_caret{ -1, 0 };
}
static void quiet_append_line_text(fz_context* ctx, fz_buffer* buffer, const quiet_visual_line& line, int first, int last)
{
    first = quiet_clamp_int(first, 0, static_cast<int>(line.chars.size()));
    last = quiet_clamp_int(last, first, static_cast<int>(line.chars.size()));
    for (int i = first; i < last; ++i)
        fz_append_rune(ctx, buffer, line.chars[static_cast<size_t>(i)].ch->c);
}

static bool quiet_line_quad(const quiet_visual_line& line, int first, int last, fz_rect& out)
{
    first = quiet_clamp_int(first, 0, static_cast<int>(line.chars.size()));
    last = quiet_clamp_int(last, first, static_cast<int>(line.chars.size()));
    bool have = false;
    for (int i = first; i < last; ++i)
    {
        fz_rect r = line.chars[static_cast<size_t>(i)].rect;
        if (!have)
        {
            out = r;
            have = true;
        }
        else
        {
            out.x0 = std::min(out.x0, r.x0);
            out.y0 = std::min(out.y0, r.y0);
            out.x1 = std::max(out.x1, r.x1);
            out.y1 = std::max(out.y1, r.y1);
        }
    }
    return have;
}

static int quiet_append_line_quads(
    const quiet_visual_line& line,
    int first,
    int last,
    fz_rect bounds,
    fz_quad* quads,
    int count,
    int max_quads)
{
    first = quiet_clamp_int(first, 0, static_cast<int>(line.chars.size()));
    last = quiet_clamp_int(last, first, static_cast<int>(line.chars.size()));
    if (last <= first)
        return count;

    fz_rect run = line.chars[static_cast<size_t>(first)].rect;
    fz_rect previous = run;
    auto flush_run = [&]()
    {
        if (count >= max_quads)
            return;
        fz_quad quad{};
        quad.ul = fz_point{ run.x0, run.y0 };
        quad.ur = fz_point{ run.x1, run.y0 };
        quad.ll = fz_point{ run.x0, run.y1 };
        quad.lr = fz_point{ run.x1, run.y1 };
        quiet_normalize_quad(quad, bounds);
        quads[count++] = quad;
    };

    for (int i = first + 1; i < last; ++i)
    {
        fz_rect r = line.chars[static_cast<size_t>(i)].rect;
        float gap = r.x0 - previous.x1;
        float split_gap = std::max(3.0f, line.size * 0.72f);
        if (gap > split_gap)
        {
            flush_run();
            run = r;
        }
        else
        {
            run.x0 = std::min(run.x0, r.x0);
            run.y0 = std::min(run.y0, r.y0);
            run.x1 = std::max(run.x1, r.x1);
            run.y1 = std::max(run.y1, r.y1);
        }
        previous = r;
        if (count >= max_quads)
            return count;
    }

    flush_run();
    return count;
}
extern "C" int quiet_pdf_select_text(
    fz_context* ctx,
    fz_document* doc,
    int page_number,
    float ax,
    float ay,
    float bx,
    float by,
    fz_quad* quads,
    int max_quads,
    char* text,
    int text_capacity)
{
    quiet_set_last_error("");
    if (!ctx || !doc || page_number < 0 || !quads || max_quads <= 0)
    {
        quiet_set_last_error("invalid quiet_pdf_select_text arguments");
        return 0;
    }
    if (text && text_capacity > 0)
        text[0] = 0;

    fz_stext_page* stext = nullptr;
    fz_buffer* text_buffer = nullptr;
    int count = 0;

    fz_try(ctx)
    {
        int page_count = fz_count_pages(ctx, doc);
        if (page_number >= page_count)
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "page out of range");

        fz_page* page = fz_load_page(ctx, doc, page_number);
        fz_rect bounds = fz_bound_page(ctx, page);
        fz_drop_page(ctx, page);

        fz_stext_options opts{};
        opts.flags = FZ_STEXT_DEHYPHENATE;
        stext = fz_new_stext_page_from_page_number(ctx, doc, page_number, &opts);

        fz_point a{
            bounds.x0 + fz_clamp(ax, 0.0f, 1.0f) * (bounds.x1 - bounds.x0),
            bounds.y0 + fz_clamp(ay, 0.0f, 1.0f) * (bounds.y1 - bounds.y0)
        };
        fz_point b{
            bounds.x0 + fz_clamp(bx, 0.0f, 1.0f) * (bounds.x1 - bounds.x0),
            bounds.y0 + fz_clamp(by, 0.0f, 1.0f) * (bounds.y1 - bounds.y0)
        };

        std::vector<quiet_visual_line> lines = quiet_build_visual_lines(stext);
        bool selectingDown = (b.y > a.y) || (std::abs(b.y - a.y) < 0.01f && b.x >= a.x);

        // In a gap between visual lines, retain the closest text span in the drag
        // direction. This avoids a single code line pulling in its neighbor when
        // glyph bounds do not fill the full visual line height.
        quiet_visual_caret caretA = quiet_caret_for_visual_point(
            lines,
            a,
            selectingDown ? quiet_gap_policy::next_line_start : quiet_gap_policy::previous_line_end);
        quiet_visual_caret caretB = quiet_caret_for_visual_point(
            lines,
            b,
            selectingDown ? quiet_gap_policy::previous_line_end : quiet_gap_policy::next_line_start);
        if (caretA.line < 0 || caretB.line < 0)
            return 0;

        int lineA = caretA.line;
        int lineB = caretB.line;
        int boundaryA = caretA.boundary;
        int boundaryB = caretB.boundary;
        if (lineB < lineA || (lineB == lineA && boundaryB < boundaryA))
        {
            std::swap(lineA, lineB);
            std::swap(boundaryA, boundaryB);
        }

        text_buffer = fz_new_buffer(ctx, 1024);
        for (int lineIndex = lineA; lineIndex <= lineB && count < max_quads; ++lineIndex)
        {
            const quiet_visual_line& line = lines[static_cast<size_t>(lineIndex)];
            if (line.chars.empty()) continue;
            int first = lineIndex == lineA ? boundaryA : 0;
            int last = lineIndex == lineB ? boundaryB : static_cast<int>(line.chars.size());
            if (last <= first) continue;

            count = quiet_append_line_quads(line, first, last, bounds, quads, count, max_quads);

            if (text_buffer)
            {
                if (lineIndex > lineA)
                    fz_append_byte(ctx, text_buffer, '\n');
                quiet_append_line_text(ctx, text_buffer, line, first, last);
            }
        }

        if (text && text_capacity > 0 && text_buffer)
        {
            fz_terminate_buffer(ctx, text_buffer);
            quiet_copy_text(text, static_cast<size_t>(text_capacity), fz_string_from_buffer(ctx, text_buffer));
        }
    }
    fz_always(ctx)
    {
        if (text_buffer) fz_drop_buffer(ctx, text_buffer);
        if (stext) fz_drop_stext_page(ctx, stext);
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        count = 0;
        if (text && text_capacity > 0)
            text[0] = 0;
    }

    return count;
}
extern "C" int quiet_pdf_find_image_at_point(
    fz_context* ctx,
    fz_document* doc,
    int page_number,
    float x,
    float y,
    fz_rect* out_rect)
{
    quiet_set_last_error("");
    if (!ctx || !doc || page_number < 0 || !out_rect)
    {
        quiet_set_last_error("invalid quiet_pdf_find_image_at_point arguments");
        return 0;
    }

    fz_stext_page* stext = nullptr;
    int found = 0;
    out_rect->x0 = out_rect->y0 = out_rect->x1 = out_rect->y1 = 0.0f;

    fz_try(ctx)
    {
        int page_count = fz_count_pages(ctx, doc);
        if (page_number >= page_count)
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "page out of range");

        fz_page* page = fz_load_page(ctx, doc, page_number);
        fz_rect bounds = fz_bound_page(ctx, page);
        fz_drop_page(ctx, page);

        fz_stext_options opts{};
        opts.flags = FZ_STEXT_DEHYPHENATE | FZ_STEXT_PRESERVE_IMAGES;
        stext = fz_new_stext_page_from_page_number(ctx, doc, page_number, &opts);

        fz_point point{
            bounds.x0 + fz_clamp(x, 0.0f, 1.0f) * (bounds.x1 - bounds.x0),
            bounds.y0 + fz_clamp(y, 0.0f, 1.0f) * (bounds.y1 - bounds.y0)
        };

        float best_area = 0.0f;
        for (fz_stext_block* block = stext ? stext->first_block : nullptr; block; block = block->next)
        {
            if (block->type != FZ_STEXT_BLOCK_IMAGE)
                continue;

            fz_rect bbox = block->bbox;
            if (point.x < bbox.x0 || point.x > bbox.x1 || point.y < bbox.y0 || point.y > bbox.y1)
                continue;

            float area = (bbox.x1 - bbox.x0) * (bbox.y1 - bbox.y0);
            if (!found || area < best_area)
            {
                *out_rect = bbox;
                best_area = area;
                found = 1;
            }
        }

        if (found)
        {
            float width = bounds.x1 - bounds.x0;
            float height = bounds.y1 - bounds.y0;
            out_rect->x0 = (out_rect->x0 - bounds.x0) / width;
            out_rect->x1 = (out_rect->x1 - bounds.x0) / width;
            out_rect->y0 = (out_rect->y0 - bounds.y0) / height;
            out_rect->y1 = (out_rect->y1 - bounds.y0) / height;
        }
    }
    fz_always(ctx)
    {
        if (stext) fz_drop_stext_page(ctx, stext);
    }
    fz_catch(ctx)
    {
        quiet_set_last_error(fz_caught_message(ctx));
        found = 0;
        out_rect->x0 = out_rect->y0 = out_rect->x1 = out_rect->y1 = 0.0f;
    }

    return found;
}
