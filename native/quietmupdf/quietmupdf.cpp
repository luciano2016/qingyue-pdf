#include "mupdf/fitz.h"
#include "mupdf/pdf.h"
#include <ctime>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <vector>

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

extern "C" int quietmupdf_anchor()
{
    return 0;
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
    fz_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    fz_irect bbox{ x0, y0, x1, y1 };

    fz_try(ctx)
    {
        page = fz_load_page(ctx, doc, number);
        pix = fz_new_pixmap_with_bbox(ctx, cs, bbox, nullptr, alpha);
        fz_clear_pixmap_with_value(ctx, pix, alpha ? 0 : 255);
        dev = fz_new_draw_device(ctx, ctm, pix);
        fz_run_page_contents(ctx, page, dev, fz_identity, nullptr);
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

                        fz_rect rect = fz_invalid_rect;
                        int quad_count = pdf_annot_quad_point_count(ctx, annot);
                        if (quad_count > 0)
                        {
                            for (int q = 0; q < quad_count; ++q)
                            {
                                fz_rect qr = fz_rect_from_quad(pdf_annot_quad_point(ctx, annot, q));
                                if (q == 0) rect = qr;
                                else
                                {
                                    if (qr.x0 < rect.x0) rect.x0 = qr.x0;
                                    if (qr.y0 < rect.y0) rect.y0 = qr.y0;
                                    if (qr.x1 > rect.x1) rect.x1 = qr.x1;
                                    if (qr.y1 > rect.y1) rect.y1 = qr.y1;
                                }
                            }
                        }
                        else
                        {
                            rect = pdf_annot_rect(ctx, annot);
                        }

                        if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                            continue;

                        quiet_pdf_annotation_info& item = out[count];
                        std::memset(&item, 0, sizeof(item));
                        item.page = page_index;
                        item.kind = kind;
                        item.x0 = fz_clamp((rect.x0 - bounds.x0) / page_w, 0.0f, 1.0f);
                        item.y0 = fz_clamp((rect.y0 - bounds.y0) / page_h, 0.0f, 1.0f);
                        item.x1 = fz_clamp((rect.x1 - bounds.x0) / page_w, 0.0f, 1.0f);
                        item.y1 = fz_clamp((rect.y1 - bounds.y0) / page_h, 0.0f, 1.0f);
                        float color[4] = { 1.0f, 0.82f, 0.0f, 1.0f };
                        int n = 0;
                        pdf_annot_color(ctx, annot, &n, color);
                        item.color_r = n > 0 ? color[0] : 1.0f;
                        item.color_g = n > 1 ? color[1] : 0.82f;
                        item.color_b = n > 2 ? color[2] : 0.0f;
                        item.opacity = pdf_annot_opacity(ctx, annot);
                        int64_t date = pdf_annot_creation_date(ctx, annot);
                        if (date <= 0) date = pdf_annot_modification_date(ctx, annot);
                        item.created_at_ms = date > 0 ? date * 1000 : 0;
                        quiet_copy_text(item.author, sizeof(item.author), pdf_annot_author(ctx, annot));
                        quiet_copy_text(item.contents, sizeof(item.contents), pdf_annot_contents(ctx, annot));
                        ++count;
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

static fz_rect quiet_rect_from_quad(fz_quad q)
{
    fz_rect r;
    r.x0 = std::min({ q.ul.x, q.ur.x, q.ll.x, q.lr.x });
    r.y0 = std::min({ q.ul.y, q.ur.y, q.ll.y, q.lr.y });
    r.x1 = std::max({ q.ul.x, q.ur.x, q.ll.x, q.lr.x });
    r.y1 = std::max({ q.ul.y, q.ur.y, q.ll.y, q.lr.y });
    return r;
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

        fz_point a{
            bounds.x0 + fz_clamp(ax, 0.0f, 1.0f) * (bounds.x1 - bounds.x0),
            bounds.y0 + fz_clamp(ay, 0.0f, 1.0f) * (bounds.y1 - bounds.y0)
        };
        fz_point b{
            bounds.x0 + fz_clamp(bx, 0.0f, 1.0f) * (bounds.x1 - bounds.x0),
            bounds.y0 + fz_clamp(by, 0.0f, 1.0f) * (bounds.y1 - bounds.y0)
        };

        count = fz_highlight_selection(ctx, stext, a, b, quads, max_quads);
        for (int i = 0; i < count; ++i)
        {
            quads[i].ul.x = (quads[i].ul.x - bounds.x0) / (bounds.x1 - bounds.x0);
            quads[i].ur.x = (quads[i].ur.x - bounds.x0) / (bounds.x1 - bounds.x0);
            quads[i].ll.x = (quads[i].ll.x - bounds.x0) / (bounds.x1 - bounds.x0);
            quads[i].lr.x = (quads[i].lr.x - bounds.x0) / (bounds.x1 - bounds.x0);
            quads[i].ul.y = (quads[i].ul.y - bounds.y0) / (bounds.y1 - bounds.y0);
            quads[i].ur.y = (quads[i].ur.y - bounds.y0) / (bounds.y1 - bounds.y0);
            quads[i].ll.y = (quads[i].ll.y - bounds.y0) / (bounds.y1 - bounds.y0);
            quads[i].lr.y = (quads[i].lr.y - bounds.y0) / (bounds.y1 - bounds.y0);
        }

        if (text && text_capacity > 0)
        {
            text[0] = 0;
            char* copied = fz_copy_selection(ctx, stext, a, b, 1);
            if (copied)
            {
                size_t i = 0;
                for (; i + 1 < static_cast<size_t>(text_capacity) && copied[i]; ++i)
                    text[i] = copied[i];
                text[i] = 0;
                fz_free(ctx, copied);
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
