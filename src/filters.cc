#include "refinery/filters.h"

#include "refinery/camera.h"
#include "refinery/image.h"

namespace refinery {

namespace {
  class ScaleColorsFilterImpl {
    Image& mImage;
    const CameraData& mCameraData;

    unsigned short clamp16(int val) {
      if (val < 0) return 0;
      if (val > 0xffff) return 0xffff;
      return val;
      /*
      // Assume right-shift of signed values leaves 1s, not 0s
      const unsigned int trashBits = val >> 16;
      return trashBits ? (~trashBits >> 16) : val;
      */
    }

  public:
    ScaleColorsFilterImpl(Image& image)
        : mImage(image), mCameraData(image.cameraData()) {}

    void filter() {
      Camera::ColorConversionData colorData(mCameraData.colorConversionData());

      const unsigned int height(mImage.height());
      const unsigned int width(mImage.width());

      for (unsigned int row = 0; row < height; row++) {
        Image::RowType pix(&mImage.pixelsRow(row)[0]);

        Image::Color c = mImage.colorAtPoint(Point(row, 0));
        double multiplier = colorData.scalingMultipliers[c];
        for (unsigned int col = 0; col < width; col += 2) {
          pix[col][c] = clamp16(multiplier * pix[col][c]);
        }

        c = mImage.colorAtPoint(Point(row, 1));
        multiplier = colorData.scalingMultipliers[c];
        for (unsigned int col = 1; col < width; col += 2) {
          pix[col][c] = clamp16(multiplier * pix[col][c]);
        }
      }
    }
  };

  class ConvertToRgbFilterImpl {
    Image& mImage;
    const CameraData& mCameraData;

    unsigned short clamp16(int val) {
      // Assume right-shift of signed values leaves 1s, not 0s
      const unsigned int trashBits = val >> 16;
      return trashBits ? (~trashBits >> 16) : val;
    }

  public:
    ConvertToRgbFilterImpl(Image& image)
        : mImage(image), mCameraData(image.cameraData()) {}

    void filter() {
      Camera::ColorConversionData colorData(mCameraData.colorConversionData());

      float outToCamera[3][4]; // convert to float from double
      for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
          outToCamera[i][j] = colorData.rgbToCamera[i][j];
        }
      }

      const unsigned int height(mImage.height());
      const unsigned int width(mImage.width());
      const unsigned int nColors(mCameraData.colors());

      for (unsigned int row = 0; row < height; row++) {
        Image::RowType pixels(&mImage.pixelsRow(row)[0]);
        for (unsigned int col = 0; col < width; col++, pixels++) {
          float rgb[3] = { 0.0f, 0.0f, 0.0f };

          for (unsigned int c = 0; c < nColors; c++) {
            rgb[0] += outToCamera[0][c] * pixels[0][c];
            rgb[1] += outToCamera[1][c] * pixels[0][c];
            rgb[2] += outToCamera[2][c] * pixels[0][c];
          }

          pixels[0][0] = clamp16(rgb[0]);
          pixels[0][1] = clamp16(rgb[1]);
          pixels[0][2] = clamp16(rgb[2]);
        }
      }
    }
  };
} // namespace {}

void ScaleColorsFilter::filter(Image& image)
{
  ScaleColorsFilterImpl impl(image);
  impl.filter();
}

void ConvertToRgbFilter::filter(Image& image)
{
  ConvertToRgbFilterImpl impl(image);
  impl.filter();
}

};