#include "refinery/unpack.h"

#include "refinery/image.h"
#include "refinery/input.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include <exiv2/exif.hpp>
#include <exiv2/tags.hpp>

namespace refinery {

namespace unpack {

  using Exiv2::ExifData;
  using Exiv2::ExifKey;
  using Exiv2::Exifdatum;

  class Unpacker {
  public:
    virtual Image* unpackImage(
        std::streambuf& is, int width, int height,
        const ExifData* exifData) = 0;
  };

  class PpmUnpacker : public Unpacker {
  private:
    void unpackHeader(std::streambuf& is, Image& image)
    {
      const int HEADER_SIZE = 22; // usually overkill
      char buf[HEADER_SIZE];
      is.sgetn(buf, HEADER_SIZE);

      std::string header(buf, 22);

      std::istringstream headerIss(header);

      std::string p6;
      headerIss >> p6; // "P6"

      unsigned short width, height;
      headerIss >> width >> height;

      unsigned short maxValue;
      headerIss >> maxValue;

      is.pubseekoff(
          static_cast<int>(headerIss.tellg()) + 1 - HEADER_SIZE,
          std::ios::cur);

      image.setWidth(width);
      image.setHeight(height);
      image.setBytesPerPixel(maxValue == 65535 ? 6 : 3);
    }

    void copyShorts(
        std::streambuf& is, unsigned int nValues, unsigned short* out)
    {
      while (nValues--) {
        uint16_t msb = static_cast<unsigned char>(is.sbumpc());
        unsigned char lsb = static_cast<unsigned char>(is.sbumpc());
        *out = (msb << 8) | lsb;
        out++;
      }
    }

    void copyChars(
        std::streambuf& is, unsigned int nValues, unsigned short* out)
    {
      while (nValues--) {
        unsigned char c = static_cast<unsigned char>(is.sbumpc());
        *out = static_cast<unsigned short>(c);
        out++;
      }
    }

  public:
    virtual Image* unpackImage(
        std::streambuf& is, int width, int height, const ExifData* exifData)
    {
      std::auto_ptr<Image> image(new Image(width, height));

      unpackHeader(is, *image);

      unsigned int nValues =
          image->width() * image->height()
          * image->bytesPerPixel() / sizeof(Image::ValueType);

      image->pixels().assign(nValues, 0);
      unsigned short* shorts(static_cast<unsigned short*>(&image->pixels()[0]));

      if (image->bytesPerPixel() == 6) {
        copyShorts(is, nValues, shorts);
      } else {
        copyChars(is, nValues, shorts);
      }

      return image.release();
    }
  };

  class NefCompressedUnpacker : public Unpacker {
  protected:
    /*
     * The linearization curve, read from Exif data, is a lookup table. In goes
     * a 12-bit (or 14-bit) value; out comes the full 16 bits.
     *
     * Many of the final entries will have the same value. "max" will be set to
     * point to the first of those max-value entries.
     *
     * See http://lclevy.free.fr/nef/ to see how this is deciphered.
     */
    class LinearizationCurve {
    public:
      std::vector<unsigned short> table;
      unsigned char version0;
      unsigned char version1;
      unsigned short vpred[2][2];
      unsigned short split;
      int max;

    private:
      unsigned short bytesToShort(const unsigned char* bytes)
      {
        return static_cast<unsigned short>(bytes[0]) << 8 | bytes[1];
      }

      void fillTable(
          const std::vector<unsigned short>& rawTable, int bitsPerSample)
      {
        int tableSize = 1 << bitsPerSample;
        int stepSize = tableSize / (rawTable.size() - 1);
        table.assign(tableSize, 0);

        for (int i = 0, curStep = 0, stepPos = 0; i < tableSize; i++, stepPos++)
        {
          if (stepPos == stepSize) {
            stepPos = 0;
            curStep++;
          }

          table[i] =
              (rawTable[curStep] * (stepSize-stepPos) +
               rawTable[curStep+1] * stepPos)
              / stepSize;
        }

        for (max = tableSize - 1; table[max-1] == table[max-2]; max--) {}
      }

      void init(const ExifData& exifData, int bitsPerSample)
      {
        ExifData::const_iterator iterator(
            exifData.findKey(ExifKey("Exif.Nikon3.LinearizationTable")));
        if (iterator == exifData.end()) {
          throw std::string("Exif.Nikon3.LinearizationTable is missing");
        }

        const Exifdatum& datum(*iterator);
        std::vector<unsigned char> bytes(datum.size(), 0);
        datum.copy(&bytes[0], Exiv2::bigEndian);

        version0 = bytes[0];
        version1 = bytes[1];
        vpred[0][0] = bytesToShort(&bytes[2]);
        vpred[0][1] = bytesToShort(&bytes[4]);
        vpred[1][0] = bytesToShort(&bytes[6]);
        vpred[1][1] = bytesToShort(&bytes[8]);

        unsigned int nShorts = bytesToShort(&bytes[10]);

        std::vector<unsigned short> rawTable;
        rawTable.reserve(nShorts);
        for (unsigned int i = 12; i < 12 + nShorts * 2; i += 2) {
          rawTable.push_back(bytesToShort(&bytes[i]));
        }

        if (version0 == 0x44 && version1 == 0x20) {
          split = bytesToShort(&bytes[12 + nShorts * 2]);
        } else {
          split = 0;
        }

        this->fillTable(rawTable, bitsPerSample);
      }

    public:
      LinearizationCurve(const ExifData& exifData, int bitsPerSample)
      {
        this->init(exifData, bitsPerSample);
      }
    };

    unsigned int getBitsPerSample(const ExifData& exifData)
    {
      ExifData::const_iterator iterator(
          exifData.findKey(ExifKey("Exif.SubImage2.BitsPerSample")));
      if (iterator == exifData.end()) {
        throw std::string("Exif.SubImage2.BitsPerSample is missing");
      }
      return (*iterator).toLong();
    }

    unsigned int getDataOffset(const ExifData& exifData)
    {
      ExifData::const_iterator iterator(
          exifData.findKey(ExifKey("Exif.SubImage2.StripOffsets")));
      if (iterator == exifData.end()) {
        throw std::string("Exif.SubImage2.StripOffsets is missing");
      }
      return (*iterator).toLong();
    }

    unsigned int getFilters(const ExifData& exifData)
    {
      /*
       * If Exif.SubImage2.CFAPattern == 1 2 0 1, that means we look like this:
       *   GRGRGRGR...
       *   BGBGBGBG...
       *   GRGRGRGR...
       *   BGBGBGBG...
       *   ...
       */
      ExifData::const_iterator iterator(
          exifData.findKey(ExifKey("Exif.SubImage2.CFAPattern")));
      if (iterator == exifData.end()) {
        throw std::string("Exif.SubImage2.CFAPattern is missing");
      }
      const Exifdatum& datum(*iterator);

      std::vector<unsigned char> bytes(datum.size(), 0);
      datum.copy(&bytes[0], Exiv2::bigEndian);

      // XXX no idea if this is right--just that Nikon D5000 is 0x49494949
      unsigned int filters =
          ((bytes[0] << 6) & 0xc0)
          | ((bytes[2] << 4) & 0x30)
          | ((bytes[1] << 2) & 0xc)
          | (bytes[3] & 0x3);

      filters = filters << 24 | filters << 16 | filters << 8 | filters;

      return filters;
    }

    /*
     * Returns a Huffman decoder from the input stream.
     *
     * Seek with the input stream, then read with the decoder.
     */
    virtual HuffmanDecoder* getDecoder(
        std::streambuf& is, const ExifData& exifData) = 0;

    /*
     * For files with a "split" (after the linearization table in Exif data),
     * this is what to use after the split.
     */
    virtual HuffmanDecoder* getDecoder2(
        std::streambuf& is, const ExifData& exifData) {
      return getDecoder(is, exifData);
    }

    /*
     * Nikons use six Huffman tables. getDecoder() and getDecoder2() can call
     * this method to create the right one.
     */
    HuffmanDecoder* createDecoder(std::streambuf& is, int key)
    {
      static const unsigned char NIKON_TREE[][32] = { // dcraw.c
        { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,  /* 12-bit lossy */
          5,4,3,6,2,7,1,0,8,9,11,10,12 },
        { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,  /* 12-bit lossy after split */
          0x39,0x5a,0x38,0x27,0x16,5,4,3,2,1,0,11,12,12 },
        { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,  /* 12-bit lossless */
          5,4,6,3,7,2,8,1,9,0,10,11,12 },
        { 0,1,4,3,1,1,1,1,1,2,0,0,0,0,0,0,  /* 14-bit lossy */
          5,6,4,7,8,3,9,2,1,0,10,11,12,13,14 },
        { 0,1,5,1,1,1,1,1,1,1,2,0,0,0,0,0,  /* 14-bit lossy after split */
          8,0x5c,0x4b,0x3a,0x29,7,6,5,4,3,2,1,0,13,14 },
        { 0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0,  /* 14-bit lossless */
          7,6,8,5,9,4,10,3,11,12,2,0,1,13,14 } };
      return new HuffmanDecoder(is, NIKON_TREE[key]);
    }

  public:
    virtual Image* unpackImage(
        std::streambuf& is, int width, int height, const ExifData* exifDataPtr)
    {
      const ExifData& exifData(*exifDataPtr);

      int bitsPerSample = getBitsPerSample(exifData);

      LinearizationCurve curve(exifData, bitsPerSample);

      const std::vector<unsigned short>& curveTable(curve.table);
      const unsigned short curveSize(curveTable.size());
      unsigned short max(curve.max);

      int left_margin = 0;

      unsigned short vpred[2][2];
      unsigned short hpred[2];

      vpred[0][0] = curve.vpred[0][0];
      vpred[0][1] = curve.vpred[0][1];
      vpred[1][0] = curve.vpred[1][0];
      vpred[1][1] = curve.vpred[1][1];

      unsigned int filters = getFilters(exifData);

      is.pubseekoff(getDataOffset(exifData), std::ios::beg);

      std::auto_ptr<Image> imagePtr(new Image(width, height));
      Image& image(*imagePtr);

      image.setFilters(filters);
      image.setBytesPerPixel(6);
      image.pixels().assign(width * height * 3, 0);

      std::auto_ptr<HuffmanDecoder> decoder(getDecoder(is, exifData));
      int min = 0;
      for (int row = 0; row < height; row++) {
        Image::RowType rowPixels(image.pixelsRow(row));
        Image::Color rowColors[2] = {
          image.colorAtPoint(Point(row, 0)),
          image.colorAtPoint(Point(row, 1))
        };

#if 0
        /* FIXME why isn't this working? */
        if (settings.split && row == settings.split) {
          decoder.reset(getDecoder2(is, settings));
          min = 16;
          max += 32;
        }
#endif

        for (int col = 0; col < width; col++) {
          unsigned int colIsOdd = col & 1;
          int i = decoder->nextHuffmanValue();
          int len = i & 0xf;
          int shl = i >> 4;

          uint16_t bits = decoder->nextBitsValue(len - shl);

          int diff = ((bits << 1) | 1) << shl >> 1;

          if ((diff & 1 << (len - 1)) == 0) {
            diff -= (1 << len) - !shl;
          }
          if (col < 2) {
            hpred[col] = vpred[row & 1][col] += diff;
          } else {
            hpred[colIsOdd] += diff;
          }
          if (hpred[colIsOdd] + min >= max) throw "Error";
          if (col - left_margin < width) {
            unsigned short val = std::min(hpred[colIsOdd], curveSize);

            rowPixels[col][rowColors[colIsOdd]] = curveTable[val];
          }
        }
      }

      return imagePtr.release();
    }
  };

  class NefCompressedLossy2Unpacker : public NefCompressedUnpacker {
  protected:
    virtual HuffmanDecoder* getDecoder(
        std::streambuf& is, const ExifData& exifData)
    {
      return createDecoder(is, 0);
    }

    virtual HuffmanDecoder* getDecoder2(
        std::streambuf& is, const ExifData& exifData)
    {
      return createDecoder(is, 1);
    }
  };

  class UnpackerFactory {
  public:
    static Unpacker* createUnpacker(
        const char* mimeType, const ExifData* exifData)
    {
      std::string sMimeType(mimeType);

      if (sMimeType == "image/x-portable-pixmap") {
        return new PpmUnpacker();
      } else if (sMimeType == "image/tiff") {
        // for now...
        return new NefCompressedLossy2Unpacker();
      }

      return 0;
    }
  };

}

Image* ImageReader::readImage(
    std::streambuf& istream, const char* mimeType,
    int width, int height, const Exiv2::ExifData* exifData)
{
  std::auto_ptr<unpack::Unpacker> unpacker(
      unpack::UnpackerFactory::createUnpacker(mimeType, exifData));

  std::auto_ptr<Image> ret(
      unpacker->unpackImage(istream, width, height, exifData));

  unsigned int filters(ret->filters());
  ret->setFilters(filters & (~((filters & 0x55555555) << 1)));

  return ret.release();
}

} // namespace refinery
