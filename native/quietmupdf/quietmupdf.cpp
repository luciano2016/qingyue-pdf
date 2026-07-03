#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

struct quiet_pdf_annotation
{
    int page;
    int kind;
    float x0;
    float y0;
    float x1;
    float y1;
    const char* contents;
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
        fz_run_page(ctx, page, dev, fz_identity, nullptr);
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

extern "C" int quiet_pdf_save_annotations(
    fz_context* ctx,
    fz_document* fzdoc,
    const char* filename,
    const quiet_pdf_annotation* annotations,
    int annotation_count)
{
    quiet_set_last_error("");
    if (!ctx || !fzdoc || !filename || !annotations || annotation_count < 0)
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
                    if (input.contents && input.contents[0])
                        pdf_set_annot_contents(ctx, annot, input.contents);

                    if (type == PDF_ANNOT_HIGHLIGHT)
                    {
                        float color[3] = { 1.0f, 0.82f, 0.24f };
                        pdf_set_annot_color(ctx, annot, 3, color);
                        pdf_set_annot_opacity(ctx, annot, 0.45f);
                    }
                    else if (type == PDF_ANNOT_UNDERLINE)
                    {
                        float color[3] = { 0.16f, 0.64f, 0.42f };
                        pdf_set_annot_color(ctx, annot, 3, color);
                    }
                    else
                    {
                        float color[3] = { 0.84f, 0.28f, 0.28f };
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
