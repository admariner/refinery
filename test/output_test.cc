#include <gtest/gtest.h>

#include "refinery/output.h"

#include <memory>
#include <sstream>
#include <string>

#include "refinery/image.h"
#include "refinery/input.h"
#include "refinery/unpack.h"

namespace {

class ImageWriterTest : public ::testing::Test {
};

TEST(ImageWriterTest, WritePpm) {
  // Assume PPM input works and load up an Image
  refinery::FileInputStream fis("./test/files/nikon_d5000_225x75_sample_ahd16.ppm");
  refinery::UnpackSettings settings; // almost ignored. FIXME improve API
  settings.format = refinery::UnpackSettings::FORMAT_PPM;

  refinery::ImageReader reader;
  std::auto_ptr<refinery::Image> imagePtr(reader.readImage(fis, settings));
  refinery::Image& image(*imagePtr);

  // Now we have "image" so the test can begin.
  std::ostringstream out(std::ios::binary | std::ios::out);

  refinery::ImageWriter writer;
  writer.writeImage(image, out, "PPM");

  std::string s(out.str());
  EXPECT_EQ("P6\n", s.substr(0, 3));
  EXPECT_EQ("225 75\n", s.substr(3, 7));
  EXPECT_EQ("65535\n", s.substr(10, 6));
  EXPECT_EQ(101266, s.size());

  EXPECT_EQ(209, static_cast<unsigned char>(s.at(17))) << "first byte of data";
  EXPECT_EQ(76, static_cast<unsigned char>(s.at(101265))) << "last byte of data";
}

} // namespace