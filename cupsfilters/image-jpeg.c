/*
 *   JPEG image routines for CUPS.
 *
 *   Copyright 2007-2011 by Apple Inc.
 *   Copyright 1993-2007 by Easy Software Products.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Apple Inc. and are protected by Federal copyright
 *   law.  Distribution and use rights are outlined in the file "COPYING"
 *   which should have been included with this file.
 *
 * Contents:
 *
 *   _cupsImageReadJPEG() - Read a JPEG image file.
 */

/*
 * Include necessary headers...
 */

#include "image-private.h"

#  include <libexif/exif-data.h>
#ifdef HAVE_LIBJPEG
#include <jpeglib.h> /* JPEG/JFIF image definitions */


/*
 * '_cupsImageReadJPEG()' - Read a JPEG image file.
 */

int /* O  - Read status */
_cupsImageReadJPEG(
    cups_image_t *img,        /* IO - cupsImage */
    FILE *fp,                 /* I  - cupsImage file */
    cups_icspace_t primary,   /* I  - Primary choice for colorspace */
    cups_icspace_t secondary, /* I  - Secondary choice for colorspace */
    int saturation,           /* I  - Color saturation (%) */
    int hue,                  /* I  - Color hue (degrees) */
    const cups_ib_t *lut)     /* I  - Lookup table for gamma/brightness */
{
  struct jpeg_decompress_struct cinfo; /* Decompressor info */
  struct jpeg_error_mgr jerr;          /* Error handler info */
  cups_ib_t *in,                       /* Input pixels */
      *out;                            /* Output pixels */
  jpeg_saved_marker_ptr marker;        /* Pointer to marker data */
  int psjpeg = 0;                      /* Non-zero if Photoshop CMYK JPEG */
  static const char *cspaces[] =
      {/* JPEG colorspaces... */
       "JCS_UNKNOWN",
       "JCS_GRAYSCALE",
       "JCS_RGB",
       "JCS_YCbCr",
       "JCS_CMYK",
       "JCS_YCCK"};

  (void)cspaces;

  /*
   * Read the JPEG header...
   */

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_save_markers(&cinfo, JPEG_APP0 + 14, 0xffff); /* Adobe JPEG */
  jpeg_stdio_src(&cinfo, fp);
  jpeg_read_header(&cinfo, 1);

  /*
   * Parse any Adobe APPE data embedded in the JPEG file.  Since Adobe doesn't
   * bother following standards, we have to invert the CMYK JPEG data written by
   * Adobe apps...
   */

  for (marker = cinfo.marker_list; marker; marker = marker->next)
    if (marker->marker == (JPEG_APP0 + 14) && marker->data_length >= 12 &&
        !memcmp(marker->data, "Adobe", 5))
    {
      DEBUG_puts("DEBUG: Adobe CMYK JPEG detected (inverting color values)\n");
      psjpeg = 1;
    }

  cinfo.quantize_colors = 0;

  DEBUG_printf(("DEBUG: num_components = %d\n", cinfo.num_components));
  DEBUG_printf(("DEBUG: jpeg_color_space = %s\n",
                cspaces[cinfo.jpeg_color_space]));

  if (cinfo.num_components == 1)
  {
    DEBUG_puts("DEBUG: Converting image to grayscale...\n");

    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.out_color_components = 1;
    cinfo.output_components = 1;

    img->colorspace = secondary;
  }
  else if (cinfo.num_components == 4)
  {
    DEBUG_puts("DEBUG: Converting image to CMYK...\n");

    cinfo.out_color_space = JCS_CMYK;
    cinfo.out_color_components = 4;
    cinfo.output_components = 4;

    img->colorspace = (primary == CUPS_IMAGE_RGB_CMYK) ? CUPS_IMAGE_CMYK : primary;
  }
  else
  {
    DEBUG_puts("DEBUG: Converting image to RGB...\n");

    cinfo.out_color_space = JCS_RGB;
    cinfo.out_color_components = 3;
    cinfo.output_components = 3;

    img->colorspace = (primary == CUPS_IMAGE_RGB_CMYK) ? CUPS_IMAGE_RGB : primary;
  }

  jpeg_calc_output_dimensions(&cinfo);

  if (cinfo.output_width <= 0 || cinfo.output_width > CUPS_IMAGE_MAX_WIDTH ||
      cinfo.output_height <= 0 || cinfo.output_height > CUPS_IMAGE_MAX_HEIGHT)
  {
    DEBUG_printf(("DEBUG: Bad JPEG dimensions %dx%d!\n",
                  cinfo.output_width, cinfo.output_height));

    jpeg_destroy_decompress(&cinfo);

    fclose(fp);
    return (1);
  }

  img->xsize = cinfo.output_width;
  img->ysize = cinfo.output_height;

  if (cinfo.X_density > 0 && cinfo.Y_density > 0 && cinfo.density_unit > 0)
  {
    if (cinfo.density_unit == 1)
    {
      img->xppi = cinfo.X_density;
      img->yppi = cinfo.Y_density;
    }
    else
    {
      img->xppi = (int)((float)cinfo.X_density * 2.54);
      img->yppi = (int)((float)cinfo.Y_density * 2.54);
    }

    if (img->xppi == 0 || img->yppi == 0)
    {
      DEBUG_printf(("DEBUG: Bad JPEG image resolution %dx%d PPI.\n",
                    img->xppi, img->yppi));
      img->xppi = img->yppi = 200;
    }
  }

  int temp = _cupsImageReadEXIF(img, fp);

  DEBUG_printf(("DEBUG: JPEG image %dx%dx%d, %dx%d PPI\n",
                img->xsize, img->ysize, cinfo.output_components,
                img->xppi, img->yppi));

  cupsImageSetMaxTiles(img, 0);

  in = malloc(img->xsize * cinfo.output_components);
  out = malloc(img->xsize * cupsImageGetDepth(img));

  jpeg_start_decompress(&cinfo);

  while (cinfo.output_scanline < cinfo.output_height)
  {
    jpeg_read_scanlines(&cinfo, (JSAMPROW *)&in, (JDIMENSION)1);

    if (psjpeg && cinfo.output_components == 4)
    {
      /*
       * Invert CMYK data from Photoshop...
       */

      cups_ib_t *ptr; /* Pointer into buffer */
      int i;          /* Looping var */

      for (ptr = in, i = img->xsize * 4; i > 0; i--, ptr++)
        *ptr = 255 - *ptr;
    }

    if ((saturation != 100 || hue != 0) && cinfo.output_components == 3)
      cupsImageRGBAdjust(in, img->xsize, saturation, hue);

    if ((img->colorspace == CUPS_IMAGE_WHITE && cinfo.out_color_space == JCS_GRAYSCALE) ||
        (img->colorspace == CUPS_IMAGE_CMYK && cinfo.out_color_space == JCS_CMYK))
    {
#ifdef DEBUG
      int i, j;
      cups_ib_t *ptr;

      DEBUG_puts("DEBUG: Direct Data...\n");

      DEBUG_puts("DEBUG:");

      for (i = 0, ptr = in; i < img->xsize; i++)
      {
        DEBUG_puts(" ");
        for (j = 0; j < cinfo.output_components; j++, ptr++)
          DEBUG_printf(("%02X", *ptr & 255));
      }

      DEBUG_puts("\n");
#endif /* DEBUG */

      if (lut)
        cupsImageLut(in, img->xsize * cupsImageGetDepth(img), lut);

      _cupsImagePutRow(img, 0, cinfo.output_scanline - 1, img->xsize, in);
    }
    else if (cinfo.out_color_space == JCS_GRAYSCALE)
    {
      switch (img->colorspace)
      {
      default:
        break;

      case CUPS_IMAGE_BLACK:
        cupsImageWhiteToBlack(in, out, img->xsize);
        break;
      case CUPS_IMAGE_RGB:
        cupsImageWhiteToRGB(in, out, img->xsize);
        break;
      case CUPS_IMAGE_CMY:
        cupsImageWhiteToCMY(in, out, img->xsize);
        break;
      case CUPS_IMAGE_CMYK:
        cupsImageWhiteToCMYK(in, out, img->xsize);
        break;
      }

      if (lut)
        cupsImageLut(out, img->xsize * cupsImageGetDepth(img), lut);

      _cupsImagePutRow(img, 0, cinfo.output_scanline - 1, img->xsize, out);
    }
    else if (cinfo.out_color_space == JCS_RGB)
    {
      switch (img->colorspace)
      {
      default:
        break;

      case CUPS_IMAGE_RGB:
        cupsImageRGBToRGB(in, out, img->xsize);
        break;
      case CUPS_IMAGE_WHITE:
        cupsImageRGBToWhite(in, out, img->xsize);
        break;
      case CUPS_IMAGE_BLACK:
        cupsImageRGBToBlack(in, out, img->xsize);
        break;
      case CUPS_IMAGE_CMY:
        cupsImageRGBToCMY(in, out, img->xsize);
        break;
      case CUPS_IMAGE_CMYK:
        cupsImageRGBToCMYK(in, out, img->xsize);
        break;
      }

      if (lut)
        cupsImageLut(out, img->xsize * cupsImageGetDepth(img), lut);

      _cupsImagePutRow(img, 0, cinfo.output_scanline - 1, img->xsize, out);
    }
    else /* JCS_CMYK */
    {
      DEBUG_puts("DEBUG: JCS_CMYK\n");

      switch (img->colorspace)
      {
      default:
        break;

      case CUPS_IMAGE_WHITE:
        cupsImageCMYKToWhite(in, out, img->xsize);
        break;
      case CUPS_IMAGE_BLACK:
        cupsImageCMYKToBlack(in, out, img->xsize);
        break;
      case CUPS_IMAGE_CMY:
        cupsImageCMYKToCMY(in, out, img->xsize);
        break;
      case CUPS_IMAGE_RGB:
        cupsImageCMYKToRGB(in, out, img->xsize);
        break;
      }

      if (lut)
        cupsImageLut(out, img->xsize * cupsImageGetDepth(img), lut);

      _cupsImagePutRow(img, 0, cinfo.output_scanline - 1, img->xsize, out);
    }
  }

  free(in);
  free(out);

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  fclose(fp);

  return (0);
}

static void trim_spaces(char *buf)
{
  char *s = buf - 1;
  for (; *buf; ++buf)
  {
    if (*buf != ' ')
      s = buf;
  }
  *++s = 0; /* nul terminate the string on the first of the final spaces */
}

int _cupsImageReadEXIF(cups_image_t *img, FILE *fp)
{
  if (fp == NULL)
  {
    // printf("File Not Found!\n");
    return -1;
  }

  fseek(fp, 0L, SEEK_END);

  // calculating the size of the file
  long int res = ftell(fp);
  char buf[res + 1];

  fseek(fp, 0, SEEK_SET);
  int pos = 0;
  int c;

  while ((c = fgetc(fp)) != EOF)
  {
    buf[pos] = c;
    pos++;
  }

  // ExifData* ed = exif_data_new_from_file(argv[1]);
  ExifData *ed = exif_data_new_from_data(buf, res);

  if (ed == NULL)
  {
    DEBUG_printf(("DEBUG: No EXIF data found"));
    return 2;
  }

  printf("SACHIN THAKAN: using exif library");

  ExifIfd ifd = EXIF_IFD_0;
  ExifTag tagX = EXIF_TAG_X_RESOLUTION;
  ExifTag tagY = EXIF_TAG_Y_RESOLUTION;

  ExifEntry *entryX = exif_content_get_entry(ed->ifd[ifd], tagX);
  ExifEntry *entryY = exif_content_get_entry(ed->ifd[ifd], tagY);
  if (entryX)
  {
    char buf1[1024];

    /* Get the contents of the tag in human-readable form */
    exif_entry_get_value(entryX, buf1, sizeof(buf));

    trim_spaces(buf1);
    if (*buf1)
    {
      int xRes;
      sscanf(buf1, "%d", &xRes);
      img->xppi = xRes;
    }
  }

  if (entryY)
  {
    char buf2[1024];

    /* Get the contents of the tag in human-readable form */
    exif_entry_get_value(entryY, buf2, sizeof(buf));

    trim_spaces(buf2);
    if (*buf2)
    {
      int yRes;
      sscanf(buf2, "%d", &yRes);
      img->yppi = yRes;
    }
  }

  return 1;
}
#endif /* HAVE_LIBJPEG */
