/* solana_img.h: the launcher's art (included once by solana.c).
 *
 * The PNGs in assets/png are built into the exe as RCDATA resources (solana.rc).
 * They are decoded with WIC (part of Windows), scaled with its high-quality
 * (Fant) scaler to the size they are drawn at for the current DPI, optionally
 * tinted (the white icons), and drawn with AlphaBlend. Scaled copies are kept
 * until the DPI changes. */
#define COBJMACROS
#include <wincodec.h>

/* resource ids (solana.rc) */
enum {
    IMG_BANNER = 100, IMG_WORDMARK, IMG_WORDMARK_TEXT, IMG_EMPTY, IMG_BGTILE,
    IMG_I_ADD = 120, IMG_I_BUILD, IMG_I_CANCEL, IMG_I_FOLDER, IMG_I_LOG, IMG_I_PLAY, IMG_I_REBUILD, IMG_I_REMOVE, IMG_I_SCAN, IMG_I_SETTINGS,
    IMG_PS_CROSS = 140, IMG_PS_CIRCLE, IMG_PS_SQUARE, IMG_PS_TRIANGLE, IMG_PS_L1, IMG_PS_R1, IMG_PS_L2, IMG_PS_R2, IMG_PS_START, IMG_PS_SELECT,
    IMG_XB_A = 160, IMG_XB_B, IMG_XB_X, IMG_XB_Y, IMG_XB_LB, IMG_XB_RB, IMG_XB_LT, IMG_XB_RT, IMG_XB_MENU, IMG_XB_VIEW,
};

static IWICImagingFactory *wic;
typedef struct { int id, w, h, tinted; COLORREF tint; HBITMAP bmp; } img_t;
#define NIMG 160
static img_t imgs[NIMG];
static int nimgs;

static HBRUSH tile_br; static int tile_sz;
static void img_flush(void)   /* (the DPI changed) */
{
    if (tile_br) { DeleteObject(tile_br); tile_br = 0; }
    for (int i = 0; i < nimgs; i++) if (imgs[i].bmp) DeleteObject(imgs[i].bmp);
    nimgs = 0;
}
/* the resource's natural size */
static IWICBitmapFrameDecode *img_frame(int id)
{
    if (!wic && FAILED(CoCreateInstance(&CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic))) return 0;
    HRSRC r = FindResourceW(0, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return 0;
    HGLOBAL g = LoadResource(0, r);
    void *p = g ? LockResource(g) : 0;
    DWORD n = SizeofResource(0, r);
    if (!p || !n) return 0;
    IWICStream *st = 0; IWICBitmapDecoder *dec = 0; IWICBitmapFrameDecode *fr = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(wic, &st)) && SUCCEEDED(IWICStream_InitializeFromMemory(st, (BYTE *)p, n)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(wic, (IStream *)st, 0, WICDecodeMetadataCacheOnDemand, &dec)))
        IWICBitmapDecoder_GetFrame(dec, 0, &fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    return fr;
}
static int img_size(int id, int *w, int *h)
{
    static int cid[64], cw[64], ch[64], nc;
    for (int i = 0; i < nc; i++) if (cid[i] == id) { *w = cw[i]; *h = ch[i]; return 1; }
    IWICBitmapFrameDecode *fr = img_frame(id);
    if (!fr) return 0;
    UINT a, b; IWICBitmapFrameDecode_GetSize(fr, &a, &b); IWICBitmapFrameDecode_Release(fr);
    if (nc < 64) { cid[nc] = id; cw[nc] = (int)a; ch[nc] = (int)b; nc++; }
    *w = (int)a; *h = (int)b;
    return 1;
}
/* a premultiplied 32-bit DIB of the image at w x h (tinted: its alpha in that colour) */
static HBITMAP img_get(int id, int w, int h, int tinted, COLORREF tint)
{
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return 0;
    for (int i = 0; i < nimgs; i++)
        if (imgs[i].id == id && imgs[i].w == w && imgs[i].h == h && imgs[i].tinted == tinted && (!tinted || imgs[i].tint == tint)) return imgs[i].bmp;
    IWICBitmapFrameDecode *fr = img_frame(id);
    if (!fr) return 0;
    IWICBitmapScaler *sc = 0; IWICFormatConverter *cv = 0; HBITMAP bmp = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(wic, &sc)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)fr, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(wic, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)sc, &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void *bits = 0;
        bmp = CreateDIBSection(0, &bi, DIB_RGB_COLORS, &bits, 0, 0);
        if (bmp && SUCCEEDED(IWICFormatConverter_CopyPixels(cv, 0, (UINT)w * 4, (UINT)(w * h * 4), (BYTE *)bits))) {
            if (tinted) {
                unsigned *px = bits, r = GetRValue(tint), g = GetGValue(tint), b = GetBValue(tint);
                for (int i = 0; i < w * h; i++) { unsigned a = px[i] >> 24; px[i] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255); }
            }
        } else if (bmp) { DeleteObject(bmp); bmp = 0; }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (sc) IWICBitmapScaler_Release(sc);
    IWICBitmapFrameDecode_Release(fr);
    if (!bmp) return 0;
    if (nimgs == NIMG) img_flush();
    imgs[nimgs].id = id; imgs[nimgs].w = w; imgs[nimgs].h = h; imgs[nimgs].tinted = tinted; imgs[nimgs].tint = tint; imgs[nimgs].bmp = bmp; nimgs++;
    return bmp;
}
/* part (sx, sy, sw, sh) of the image scaled to w x h, at (x, y) */
static int img_blit(HDC dc, int id, int w, int h, int sx, int sy, int sw, int sh, int x, int y, int tinted, COLORREF tint, int alpha)
{
    HBITMAP b = img_get(id, w, h, tinted, tint);
    if (!b) return 0;
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(m, b);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA };
    AlphaBlend(dc, x, y, sw, sh, m, sx, sy, sw, sh, bf);
    SelectObject(m, old); DeleteDC(m);
    return 1;
}
static int img_draw(HDC dc, int id, int x, int y, int w, int h) { return img_blit(dc, id, w, h, 0, 0, w, h, x, y, 0, 0, 255); }
static int img_icon(HDC dc, int id, int x, int y, int sz, COLORREF c) { return img_blit(dc, id, sz, sz, 0, 0, sz, sz, x, y, 1, c, 255); }
/* the image filling r (cropped, never stretched): anchored right (ax = 1) or centred (0.5), centred vertically */
static int img_cover(HDC dc, int id, RECT r, double ax)
{
    int iw, ih, w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || !img_size(id, &iw, &ih)) return 0;
    double s = (double)w / iw; if ((double)h / ih > s) s = (double)h / ih;
    int sw = (int)(iw * s + 0.999), sh = (int)(ih * s + 0.999);
    {   /* the banner is resized with the window: keep only the latest size of it */
        for (int i = 0; i < nimgs; i++) if (imgs[i].id == id && !imgs[i].tinted && (imgs[i].w != sw || imgs[i].h != sh)) { DeleteObject(imgs[i].bmp); imgs[i] = imgs[--nimgs]; i--; }
    }
    return img_blit(dc, id, sw, sh, (int)((sw - w) * ax), (sh - h) / 2, w, h, r.left, r.top, 0, 0, 255);
}
/* a pattern brush of the tile at the current DPI (0 if there is none) */
static HBRUSH img_tile_brush(int id, int size)
{
    if (tile_br && tile_sz == size) return tile_br;
    if (tile_br) { DeleteObject(tile_br); tile_br = 0; }
    HBITMAP b = img_get(id, size, size, 0, 0);
    if (b) { tile_br = CreatePatternBrush(b); tile_sz = size; }
    return tile_br;
}
