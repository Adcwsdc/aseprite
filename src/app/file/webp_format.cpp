// Aseprite
// Copyright (C) 2015 Gabriel Rauter
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/console.h"
#include "app/context.h"
#include "app/document.h"
#include "app/file/file.h"
#include "app/file/file_format.h"
#include "app/file/format_options.h"
#include "app/ini_file.h"
#include "base/file_handle.h"
#include "doc/doc.h"

#include "generated_webp_options.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>


//include webp librarys
#include <webp/decode.h>
#include <webp/encode.h>

namespace app {

using namespace base;

class WebPFormat : public FileFormat {
  // Data for WEBP files TODO make this better OOP like GIF_OPTIONS
  class WebPOptions : public FormatOptions {
  public:
    WebPOptions(): lossless(1), quality(75), method(6), image_hint(WEBP_HINT_DEFAULT), image_preset(WEBP_PRESET_DEFAULT) {};
    int lossless;           // Lossless encoding (0=lossy(default), 1=lossless).
    float quality;          // between 0 (smallest file) and 100 (biggest)
    int method;             // quality/speed trade-off (0=fast, 9=slower-better)
    WebPImageHint image_hint;  // Hint for image type (lossless only for now).
    WebPPreset image_preset;  // Image Preset for lossy webp.
  };

  const char* onGetName() const { return "webp"; }
  const char* onGetExtensions() const { return "webp"; }
  int onGetFlags() const {
    return
      FILE_SUPPORT_LOAD |
      FILE_SUPPORT_SAVE |
      FILE_SUPPORT_RGB |
      FILE_SUPPORT_RGBA |
      FILE_SUPPORT_SEQUENCES |
      FILE_SUPPORT_GET_FORMAT_OPTIONS;
  }

  bool onLoad(FileOp* fop) override;
#ifdef ENABLE_SAVE
  bool onSave(FileOp* fop) override;
#endif

base::SharedPtr<FormatOptions> onGetFormatOptions(FileOp* fop) override;
};

FileFormat* CreateWebPFormat()
{
  return new WebPFormat;
}

const std::pair<VP8StatusCode, std::string> dec_error_map_data[] = {
    std::make_pair(VP8_STATUS_OK, ""),
    std::make_pair(VP8_STATUS_OUT_OF_MEMORY, "out of memory"),
    std::make_pair(VP8_STATUS_INVALID_PARAM, "invalid parameters"),
    std::make_pair(VP8_STATUS_BITSTREAM_ERROR, "bitstream error"),
    std::make_pair(VP8_STATUS_UNSUPPORTED_FEATURE, "unsupported feature"),
    std::make_pair(VP8_STATUS_SUSPENDED, "suspended"),
    std::make_pair(VP8_STATUS_USER_ABORT, "user aborted"),
    std::make_pair(VP8_STATUS_NOT_ENOUGH_DATA, "not enough data")
};

const std::map<VP8StatusCode, std::string> WebPDecodingErrorMap(dec_error_map_data, dec_error_map_data + sizeof dec_error_map_data / sizeof dec_error_map_data[0]);

bool WebPFormat::onLoad(FileOp* fop)
{
  FileHandle handle(open_file_with_exception(fop->filename, "rb"));
  FILE* fp = handle.get();

  if (fseek(fp, 0, SEEK_END) != 0) {
    fop_error(fop, "Error while getting WebP file size for %s\n", fop->filename.c_str());
    return false;
  }

  long len = ftell(fp);
  rewind(fp);

  if (len < 4) {
    fop_error(fop, "%s is corrupt or not a WebP file\n", fop->filename.c_str());
    return false;
  }

  std::vector<uint8_t> buf(len);
  auto* data = &buf.front();

  if (!fread(data, sizeof(uint8_t), len, fp)) {
    fop_error(fop, "Error while writing to %s to memory\n", fop->filename.c_str());
    return false;
  }

  WebPDecoderConfig config;
  if (!WebPInitDecoderConfig(&config)) {
    fop_error(fop, "LibWebP version mismatch %s\n", fop->filename.c_str());
    return false;
  }

  if (WebPGetFeatures(data, len, &config.input) != VP8_STATUS_OK) {
    fop_error(fop, "Bad bitstream in %s\n", fop->filename.c_str());
    return false;
  }

  fop->seq.has_alpha = config.input.has_alpha;

  auto image = fop_sequence_image(fop, IMAGE_RGB, config.input.width, config.input.height);

  config.output.colorspace = MODE_RGBA;
  config.output.u.RGBA.rgba = (uint8_t*)image->getPixelAddress(0, 0);
  config.output.u.RGBA.stride = config.input.width * sizeof(uint32_t);
  config.output.u.RGBA.size = config.input.width * config.input.height * sizeof(uint32_t);
  config.output.is_external_memory = 1;

  WebPIDecoder* idec = WebPIDecode(NULL, 0, &config);
  if (idec == NULL) {
    fop_error(fop, "Error creating WebP decoder for %s\n", fop->filename.c_str());
    return false;
  }

  auto bytes_remaining = len;
  auto bytes_read = std::max(4l, len/100l);

  while (bytes_remaining > 0) {
    VP8StatusCode status = WebPIAppend(idec, data, bytes_read);
    if (status == VP8_STATUS_OK || status == VP8_STATUS_SUSPENDED) {
      bytes_remaining -= bytes_read;
      data += bytes_read;
      if (bytes_remaining < bytes_read) bytes_read = bytes_remaining;
      fop_progress(fop, 1.0f - ((float)std::max(bytes_remaining, 0l)/(float)len));
    } else {
      fop_error(fop, "Error during decoding %s : %s\n", fop->filename.c_str(), WebPDecodingErrorMap.find(status)->second.c_str());
      WebPIDelete(idec);
      WebPFreeDecBuffer(&config.output);
      return false;
    }
    if (fop_is_stop(fop))
      break;
  }

  base::SharedPtr<WebPOptions> webPOptions = base::SharedPtr<WebPOptions>(new WebPOptions());
  fop->seq.format_options = webPOptions;
  webPOptions->lossless = std::min(config.input.format - 1, 1);

  WebPIDelete(idec);
  WebPFreeDecBuffer(&config.output);
  return true;
}

#ifdef ENABLE_SAVE
struct writerData {
  FILE* fp;
  FileOp* fop;
};

const std::pair<WebPEncodingError, std::string> enc_error_map_data[] = {
    std::make_pair(VP8_ENC_OK, ""),
    std::make_pair(VP8_ENC_ERROR_OUT_OF_MEMORY, "memory error allocating objects"),
    std::make_pair(VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY, "memory error while flushing bits"),
    std::make_pair(VP8_ENC_ERROR_NULL_PARAMETER, "a pointer parameter is NULL"),
    std::make_pair(VP8_ENC_ERROR_INVALID_CONFIGURATION, "configuration is invalid"),
    std::make_pair(VP8_ENC_ERROR_BAD_DIMENSION, "picture has invalid width/height"),
    std::make_pair(VP8_ENC_ERROR_PARTITION0_OVERFLOW, "partition is bigger than 512k"),
    std::make_pair(VP8_ENC_ERROR_PARTITION_OVERFLOW, "partition is bigger than 16M"),
    std::make_pair(VP8_ENC_ERROR_BAD_WRITE, "error while flushing bytes"),
    std::make_pair(VP8_ENC_ERROR_FILE_TOO_BIG, "file is bigger than 4G"),
    std::make_pair(VP8_ENC_ERROR_OUT_OF_MEMORY, "memory error allocating objects"),
    std::make_pair(VP8_ENC_ERROR_USER_ABORT, "abort request by user"),
    std::make_pair(VP8_ENC_ERROR_LAST, "list terminator. always last.")
};

const std::map<WebPEncodingError, std::string> WebPEncodingErrorMap(enc_error_map_data, enc_error_map_data + sizeof enc_error_map_data / sizeof enc_error_map_data[0]);

#if WEBP_ENCODER_ABI_VERSION < 0x0203
#define MAX_LEVEL 9
// Mapping between -z level and -m / -q parameter settings.
static const struct {
  uint8_t method_;
  uint8_t quality_;
} kLosslessPresets[MAX_LEVEL + 1] = {
  { 0,  0 }, { 1, 20 }, { 2, 25 }, { 3, 30 }, { 3, 50 },
  { 4, 50 }, { 4, 75 }, { 4, 90 }, { 5, 90 }, { 6, 100 }
};
int WebPConfigLosslessPreset(WebPConfig* config, int level) {
  if (config == NULL || level < 0 || level > MAX_LEVEL) return 0;
  config->lossless = 1;
  config->method = kLosslessPresets[level].method_;
  config->quality = kLosslessPresets[level].quality_;
  return 1;
}
#endif

static int ProgressReport(int percent, const WebPPicture* const pic)
{
  fop_progress(((writerData*)pic->custom_ptr)->fop, (double)percent/(double)100);
  if (fop_is_stop(((writerData*)pic->custom_ptr)->fop)) return false;
  return true;
}

static int FileWriter(const uint8_t* data, size_t data_size, const WebPPicture* const pic)
{
  return data_size ? (fwrite(data, data_size, 1, ((writerData*)pic->custom_ptr)->fp) == 1) : 1;
}


//Helper functions instead of std::stoi because of missing c++11 on mac os x
template<typename T>
static inline std::string ToString(const T& v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

template<typename T>
static inline T FromString(const std::string& str)
{
    std::istringstream ss(str);
    T ret;
    ss >> ret;
    return ret;
}

bool WebPFormat::onSave(FileOp* fop)
{
  FileHandle handle(open_file_with_exception(fop->filename, "wb"));
  FILE* fp = handle.get();

  struct writerData wd = {fp, fop};

  auto image = fop->seq.image.get();
  if (image->width() > WEBP_MAX_DIMENSION || image->height() > WEBP_MAX_DIMENSION) {
   fop_error(
     fop, "Error: WebP can only have a maximum width and height of %i but your %s has a size of %i x %i\n",
     WEBP_MAX_DIMENSION, fop->filename.c_str(), image->width(), image->height()
   );
   return false;
  }

  base::SharedPtr<WebPOptions> webp_options = fop->seq.format_options;

  WebPConfig config;

  if (webp_options->lossless) {
    if (!(WebPConfigInit(&config) && WebPConfigLosslessPreset(&config, webp_options->method))) {
     fop_error(fop, "Error for WebP Config Version for file %s\n", fop->filename.c_str());
     return false;
    }
    config.image_hint = webp_options->image_hint;
  } else {
    if (!WebPConfigPreset(&config, webp_options->image_preset, webp_options->quality)) {
     fop_error(fop, "Error for WebP Config Version for file %s\n", fop->filename.c_str());
     return false;
    }
  }

  if (!WebPValidateConfig(&config)) {
   fop_error(fop, "Error in WebP Encoder Config for file %s\n", fop->filename.c_str());
   return false;
  }

  WebPPicture pic;
  if (!WebPPictureInit(&pic)) {
    fop_error(fop, "Error for WebP Picture Version mismatch for file %s\n", fop->filename.c_str());
    return false;
  }

  pic.width = image->width();
  pic.height = image->height();
  if (webp_options->lossless) {
    pic.use_argb = true;
  }

  if (!WebPPictureAlloc(&pic)) {
    fop_error(fop, "Error for  WebP Picture memory allocations for file %s\n", fop->filename.c_str());
    return false;
  }

  if (!WebPPictureImportRGBA(&pic, (uint8_t*)image->getPixelAddress(0, 0), image->width() * sizeof(uint32_t))) {
    fop_error(fop, "Error for LibWebP Import RGBA Buffer into Picture for %s\n", fop->filename.c_str());
    WebPPictureFree(&pic);
    return false;
  }

  pic.writer = FileWriter;
  pic.custom_ptr = &wd;
  pic.progress_hook = ProgressReport;

  if (!WebPEncode(&config, &pic)) {
    fop_error(fop, "Error for LibWebP while Encoding %s: %s\n", fop->filename.c_str(), WebPEncodingErrorMap.find(pic.error_code)->second.c_str());
    WebPPictureFree(&pic);
    return false;
  }

  WebPPictureFree(&pic);
  return true;
}
#endif

// Shows the WebP configuration dialog.
base::SharedPtr<FormatOptions> WebPFormat::onGetFormatOptions(FileOp* fop)
{
  base::SharedPtr<WebPOptions> webp_options;
  if (fop->document->getFormatOptions())
    webp_options = base::SharedPtr<WebPOptions>(fop->document->getFormatOptions());

  if (!webp_options)
    webp_options.reset(new WebPOptions);

  // Non-interactive mode
  if (!fop->context || !fop->context->isUIAvailable())
    return webp_options;

  try {
    // Configuration parameters
    webp_options->quality = get_config_int("WEBP", "Quality", webp_options->quality);
    webp_options->method = get_config_int("WEBP", "Compression", webp_options->method);
    webp_options->image_hint = static_cast<WebPImageHint>(get_config_int("WEBP", "ImageHint", webp_options->image_hint));
    webp_options->image_preset = static_cast<WebPPreset>(get_config_int("WEBP", "ImagePreset", webp_options->image_preset));

    // Load the window to ask to the user the WebP options he wants.

    app::gen::WebpOptions win;
    win.lossless()->setSelected(webp_options->lossless);
    win.lossy()->setSelected(!webp_options->lossless);
    win.quality()->setValue(webp_options->quality);
    win.compression()->setValue(webp_options->method);
    win.imageHint()->setSelectedItemIndex(webp_options->image_hint);
    win.imagePreset()->setSelectedItemIndex(webp_options->image_preset);

    win.openWindowInForeground();

    if (win.getKiller() == win.ok()) {
      webp_options->quality = win.quality()->getValue();
      webp_options->method = win.compression()->getValue();
      webp_options->lossless = win.lossless()->isSelected();
      webp_options->image_hint = static_cast<WebPImageHint>(FromString<int>(win.imageHint()->getValue()));
      webp_options->image_preset = static_cast<WebPPreset>(FromString<int>(win.imagePreset()->getValue()));

      set_config_int("WEBP", "Quality", webp_options->quality);
      set_config_int("WEBP", "Compression", webp_options->method);
      set_config_int("WEBP", "ImageHint", webp_options->image_hint);
      set_config_int("WEBP", "ImagePreset", webp_options->image_preset);
    }
    else {
      webp_options.reset(NULL);
    }

    return webp_options;
  }
  catch (std::exception& e) {
    Console::showException(e);
    return base::SharedPtr<WebPOptions>(0);
  }
}

} // namespace app
