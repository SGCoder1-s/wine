/*
 * Enhanced MetaFile driver BitBlt functions
 *
 * Copyright 2002 Huw D M Davies for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "enhmetafiledrv.h"

/* Generate an EMRBITBLT, EMRSTRETCHBLT or EMRALPHABLEND record depending on the type parameter */
static BOOL emfdrv_stretchblt( PHYSDEV dev_dst, struct bitblt_coords *dst, HDC hdc_src,
                               struct bitblt_coords *src, DWORD rop, DWORD type )
{
    BITMAPINFO src_info = {{ sizeof( src_info.bmiHeader ) }};
    UINT bmi_size, emr_size, size, bpp;
    HBITMAP bitmap, blit_bitmap = NULL;
    EMRBITBLT *emr = NULL;
    BITMAPINFO *bmi;
    DIBSECTION dib;
    HDC blit_dc;
    int info_size;
    BOOL ret = FALSE;

    if (!(bitmap = GetCurrentObject( hdc_src, OBJ_BITMAP ))) return FALSE;
    if (!(info_size = GetObjectW( bitmap, sizeof(dib), &dib ))) return FALSE;

    if (info_size == sizeof(DIBSECTION))
    {
        blit_dc = hdc_src;
        blit_bitmap = bitmap;
    }
    else
    {
        unsigned char dib_info_buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
        BITMAPINFO *dib_info = (BITMAPINFO *)dib_info_buffer;
        BITMAP bmp = dib.dsBm;
        HPALETTE palette;
        void *bits;

        assert( info_size == sizeof(BITMAP) );

        dib_info->bmiHeader.biSize = sizeof(dib_info->bmiHeader);
        dib_info->bmiHeader.biWidth = bmp.bmWidth;
        dib_info->bmiHeader.biHeight = bmp.bmHeight;
        dib_info->bmiHeader.biPlanes = 1;
        dib_info->bmiHeader.biBitCount = bmp.bmBitsPixel;
        dib_info->bmiHeader.biCompression = BI_RGB;
        dib_info->bmiHeader.biSizeImage = 0;
        dib_info->bmiHeader.biXPelsPerMeter = 0;
        dib_info->bmiHeader.biYPelsPerMeter = 0;
        dib_info->bmiHeader.biClrUsed = 0;
        dib_info->bmiHeader.biClrImportant = 0;
        switch (dib_info->bmiHeader.biBitCount)
        {
        case 16:
            ((DWORD *)dib_info->bmiColors)[0] = 0xf800;
            ((DWORD *)dib_info->bmiColors)[1] = 0x07e0;
            ((DWORD *)dib_info->bmiColors)[2] = 0x001f;
            break;
        case 32:
            ((DWORD *)dib_info->bmiColors)[0] = 0xff0000;
            ((DWORD *)dib_info->bmiColors)[1] = 0x00ff00;
            ((DWORD *)dib_info->bmiColors)[2] = 0x0000ff;
            break;
        default:
            if (dib_info->bmiHeader.biBitCount > 8) break;
            if (!(palette = GetCurrentObject( hdc_src, OBJ_PAL ))) return FALSE;
            if (!GetPaletteEntries( palette, 0, 256, (PALETTEENTRY *)dib_info->bmiColors ))
                return FALSE;
        }

        if (!(blit_dc = NtGdiCreateCompatibleDC( hdc_src ))) return FALSE;
        if (!(blit_bitmap = CreateDIBSection( blit_dc, dib_info, DIB_RGB_COLORS, &bits, NULL, 0 )))
            goto err;
        if (!SelectObject( blit_dc, blit_bitmap )) goto err;
        if (!BitBlt( blit_dc, 0, 0, bmp.bmWidth, bmp.bmHeight, hdc_src, 0, 0, SRCCOPY ))
            goto err;
    }
    if (!GetDIBits( blit_dc, blit_bitmap, 0, INT_MAX, NULL, &src_info, DIB_RGB_COLORS ))
        goto err;

    bpp = src_info.bmiHeader.biBitCount;
    if (bpp <= 8)
        bmi_size = sizeof(BITMAPINFOHEADER) + (1 << bpp) * sizeof(RGBQUAD);
    else if (bpp == 16 || bpp == 32)
        bmi_size = sizeof(BITMAPINFOHEADER) + 3 * sizeof(RGBQUAD);
    else
        bmi_size = sizeof(BITMAPINFOHEADER);

    /* EMRSTRETCHBLT and EMRALPHABLEND have the same structure */
    emr_size = type == EMR_BITBLT ? sizeof(EMRBITBLT) : sizeof(EMRSTRETCHBLT);
    size = emr_size + bmi_size + src_info.bmiHeader.biSizeImage;

    if (!(emr = HeapAlloc(GetProcessHeap(), 0, size))) goto err;

    emr->emr.iType = type;
    emr->emr.nSize = size;
    emr->rclBounds.left = dst->log_x;
    emr->rclBounds.top = dst->log_y;
    emr->rclBounds.right = dst->log_x + dst->log_width - 1;
    emr->rclBounds.bottom = dst->log_y + dst->log_height - 1;
    emr->xDest = dst->log_x;
    emr->yDest = dst->log_y;
    emr->cxDest = dst->log_width;
    emr->cyDest = dst->log_height;
    emr->xSrc = src->log_x;
    emr->ySrc = src->log_y;
    if (type == EMR_STRETCHBLT || type == EMR_ALPHABLEND)
    {
        EMRSTRETCHBLT *emr_stretchblt = (EMRSTRETCHBLT *)emr;
        emr_stretchblt->cxSrc = src->log_width;
        emr_stretchblt->cySrc = src->log_height;
    }
    emr->dwRop = rop;
    NtGdiGetTransform( hdc_src, 0x204, &emr->xformSrc );
    emr->crBkColorSrc = GetBkColor( hdc_src );
    emr->iUsageSrc = DIB_RGB_COLORS;
    emr->offBmiSrc = emr_size;
    emr->cbBmiSrc = bmi_size;
    emr->offBitsSrc = emr_size + bmi_size;
    emr->cbBitsSrc = src_info.bmiHeader.biSizeImage;

    bmi = (BITMAPINFO *)((BYTE *)emr + emr->offBmiSrc);
    bmi->bmiHeader = src_info.bmiHeader;
    ret = GetDIBits( blit_dc, blit_bitmap, 0, src_info.bmiHeader.biHeight, (BYTE *)emr + emr->offBitsSrc,
                     bmi, DIB_RGB_COLORS );
    if (ret)
    {
        ret = EMFDRV_WriteRecord( dev_dst, (EMR *)emr );
        if (ret) EMFDRV_UpdateBBox( dev_dst, &emr->rclBounds );
    }

err:
    HeapFree( GetProcessHeap(), 0, emr );
    if (blit_bitmap && blit_bitmap != bitmap) DeleteObject( blit_bitmap );
    if (blit_dc && blit_dc != hdc_src) DeleteDC( blit_dc );
    return ret;
}

BOOL CDECL EMFDRV_AlphaBlend( PHYSDEV dev_dst, struct bitblt_coords *dst,
                              PHYSDEV dev_src, struct bitblt_coords *src, BLENDFUNCTION func )
{
    return emfdrv_stretchblt( dev_dst, dst, dev_src->hdc, src, *(DWORD *)&func, EMR_ALPHABLEND );
}

BOOL CDECL EMFDRV_PatBlt( PHYSDEV dev, struct bitblt_coords *dst, DWORD rop )
{
    /* FIXME: update bound rect */
    return TRUE;
}

BOOL EMFDC_PatBlt( DC_ATTR *dc_attr, INT left, INT top, INT width, INT height, DWORD rop )
{
    EMFDRV_PDEVICE *emf = dc_attr->emf;
    EMRBITBLT emr;
    BOOL ret;

    emr.emr.iType = EMR_BITBLT;
    emr.emr.nSize = sizeof(emr);
    emr.rclBounds.left = left;
    emr.rclBounds.top = top;
    emr.rclBounds.right = left + width - 1;
    emr.rclBounds.bottom = top + height - 1;
    emr.xDest = left;
    emr.yDest = top;
    emr.cxDest = width;
    emr.cyDest = height;
    emr.dwRop = rop;
    emr.xSrc = 0;
    emr.ySrc = 0;
    emr.xformSrc.eM11 = 1.0;
    emr.xformSrc.eM12 = 0.0;
    emr.xformSrc.eM21 = 0.0;
    emr.xformSrc.eM22 = 1.0;
    emr.xformSrc.eDx = 0.0;
    emr.xformSrc.eDy = 0.0;
    emr.crBkColorSrc = 0;
    emr.iUsageSrc = 0;
    emr.offBmiSrc = 0;
    emr.cbBmiSrc = 0;
    emr.offBitsSrc = 0;
    emr.cbBitsSrc = 0;

    ret = EMFDRV_WriteRecord( &emf->dev, &emr.emr );
    if(ret)
        EMFDRV_UpdateBBox( &emf->dev, &emr.rclBounds );
    return ret;
}

BOOL CDECL EMFDRV_StretchBlt( PHYSDEV devDst, struct bitblt_coords *dst,
                              PHYSDEV devSrc, struct bitblt_coords *src, DWORD rop )
{
    if (src->log_width == dst->log_width && src->log_height == dst->log_height)
        return emfdrv_stretchblt( devDst, dst, devSrc->hdc, src, rop, EMR_BITBLT );
    else
        return emfdrv_stretchblt( devDst, dst, devSrc->hdc, src, rop, EMR_STRETCHBLT );
}

INT CDECL EMFDRV_StretchDIBits( PHYSDEV dev, INT xDst, INT yDst, INT widthDst, INT heightDst,
                                INT xSrc, INT ySrc, INT widthSrc, INT heightSrc, const void *bits,
                                BITMAPINFO *info, UINT wUsage, DWORD dwRop )
{
    EMRSTRETCHDIBITS *emr;
    BOOL ret;
    UINT bmi_size, emr_size;

    /* calculate the size of the colour table */
    bmi_size = get_dib_info_size(info, wUsage);

    emr_size = sizeof (EMRSTRETCHDIBITS) + bmi_size + info->bmiHeader.biSizeImage;
    emr = HeapAlloc(GetProcessHeap(), 0, emr_size );
    if (!emr) return 0;

    /* write a bitmap info header (with colours) to the record */
    memcpy( &emr[1], info, bmi_size);

    /* write bitmap bits to the record */
    memcpy ( ( (BYTE *) (&emr[1]) ) + bmi_size, bits, info->bmiHeader.biSizeImage);

    /* fill in the EMR header at the front of our piece of memory */
    emr->emr.iType = EMR_STRETCHDIBITS;
    emr->emr.nSize = emr_size;

    emr->xDest     = xDst;
    emr->yDest     = yDst;
    emr->cxDest    = widthDst;
    emr->cyDest    = heightDst;
    emr->dwRop     = dwRop;
    emr->xSrc      = xSrc; /* FIXME: only save the piece of the bitmap needed */
    emr->ySrc      = ySrc;

    emr->iUsageSrc    = wUsage;
    emr->offBmiSrc    = sizeof (EMRSTRETCHDIBITS);
    emr->cbBmiSrc     = bmi_size;
    emr->offBitsSrc   = emr->offBmiSrc + bmi_size; 
    emr->cbBitsSrc    = info->bmiHeader.biSizeImage;

    emr->cxSrc = widthSrc;
    emr->cySrc = heightSrc;

    emr->rclBounds.left   = xDst;
    emr->rclBounds.top    = yDst;
    emr->rclBounds.right  = xDst + widthDst;
    emr->rclBounds.bottom = yDst + heightDst;

    /* save the record we just created */
    ret = EMFDRV_WriteRecord( dev, &emr->emr );
    if(ret)
        EMFDRV_UpdateBBox( dev, &emr->rclBounds );

    HeapFree(GetProcessHeap(), 0, emr);

    return ret ? heightSrc : GDI_ERROR;
}

INT CDECL EMFDRV_SetDIBitsToDevice( PHYSDEV dev, INT xDst, INT yDst, DWORD width, DWORD height,
                                    INT xSrc, INT ySrc, UINT startscan, UINT lines,
                                    LPCVOID bits, BITMAPINFO *info, UINT wUsage )
{
    EMRSETDIBITSTODEVICE* pEMR;
    DWORD bmiSize = get_dib_info_size(info, wUsage);
    DWORD size = sizeof(EMRSETDIBITSTODEVICE) + bmiSize + info->bmiHeader.biSizeImage;

    pEMR = HeapAlloc(GetProcessHeap(), 0, size);
    if (!pEMR) return 0;

    pEMR->emr.iType = EMR_SETDIBITSTODEVICE;
    pEMR->emr.nSize = size;
    pEMR->rclBounds.left = xDst;
    pEMR->rclBounds.top = yDst;
    pEMR->rclBounds.right = xDst + width - 1;
    pEMR->rclBounds.bottom = yDst + height - 1;
    pEMR->xDest = xDst;
    pEMR->yDest = yDst;
    pEMR->xSrc = xSrc;
    pEMR->ySrc = ySrc;
    pEMR->cxSrc = width;
    pEMR->cySrc = height;
    pEMR->offBmiSrc = sizeof(EMRSETDIBITSTODEVICE);
    pEMR->cbBmiSrc = bmiSize;
    pEMR->offBitsSrc = sizeof(EMRSETDIBITSTODEVICE) + bmiSize;
    pEMR->cbBitsSrc = info->bmiHeader.biSizeImage;
    pEMR->iUsageSrc = wUsage;
    pEMR->iStartScan = startscan;
    pEMR->cScans = lines;
    memcpy((BYTE*)pEMR + pEMR->offBmiSrc, info, bmiSize);
    memcpy((BYTE*)pEMR + pEMR->offBitsSrc, bits, info->bmiHeader.biSizeImage);

    if (EMFDRV_WriteRecord(dev, (EMR*)pEMR))
        EMFDRV_UpdateBBox(dev, &(pEMR->rclBounds));

    HeapFree( GetProcessHeap(), 0, pEMR);
    return lines;
}
