#include "mupdf/fitz.h"

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
