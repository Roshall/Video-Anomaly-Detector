//
// Created by lg on 9/27/25.
//
#include <iostream>
#include <fstream>
#include <opencv2/opencv.hpp>
#include <variant>

using RectOrPoly = std::variant<std::reference_wrapper<const cv::Rect>, std::reference_wrapper<const std::vector<cv::Point>>>;

/**
 * @brief Draws a filled rectangle on a YUV420p frame.
 *
 * This function operates directly on the provided cv::Mat object, which is expected
 * to contain the raw YUV420p data. It separates the Y, U, and V planes
 * and draws a rectangle/polygon on each plane accordingly.
 *
 * @param yuvFrame A cv::Mat object of type CV_8UC1, with dimensions
 * (height * 3 / 2, width), containing the YUV420p frame.
 * @param lines a Variant defining a rectangle or polygon (vector of cv::Point).
 * @param color A cv::Scalar containing the (Y, U, V) color values for the rectangle.
 * @param rectangle whether the Variant is a rectangle.
 * @param thickness thickness of lines draw in Mat
 * color[0] is Y, color[1] is U, color[2] is V.
 */
void drawOnYUV420p(cv::Mat& yuvFrame, RectOrPoly lines, const cv::Scalar& color, int thickness=5) {
  // Ensure the input Mat has the correct type and is continuous
  if (yuvFrame.type() != CV_8UC1 || !yuvFrame.isContinuous()) {
    std::cerr << "Error: Input Mat must be of type CV_8UC1 and continuous."
              << std::endl;
    return;
  }

  // Get frame dimensions from the Mat
  int width = yuvFrame.cols;
  int height = yuvFrame.rows * 2 / 3;

  // Create Mat headers for each plane without copying data.
  // This allows us to use OpenCV drawing functions directly on the planes.
  cv::Mat yPlane(height, width, CV_8UC1, yuvFrame.data);
  cv::Mat uPlane(height / 2, width / 2, CV_8UC1,
                 yuvFrame.data + width * height);
  cv::Mat vPlane(height / 2, width / 2, CV_8UC1,
                 yuvFrame.data + width * height * 5 / 4);

  // Draw on the Y (luma) plane
  if (std::holds_alternative<std::reference_wrapper<const cv::Rect>>(lines)) {
    auto rect = std::get<std::reference_wrapper<const cv::Rect>>(lines).get();
    cv::rectangle(yPlane, rect, color[0], thickness, cv::FILLED);
    // Calculate the corresponding rectangle for the chroma planes (U and V)
    // The coordinates and dimensions are halved due to 4:2:0 subsampling.
    cv::Rect uvRect(rect.x / 2, rect.y / 2, rect.width / 2, rect.height / 2);
    // Draw on the U and V (chroma) planes
    cv::rectangle(uPlane, uvRect, color[1], thickness, cv::FILLED);
    cv::rectangle(vPlane, uvRect, color[2], thickness, cv::FILLED);
} else {
  auto poly = std::get<std::reference_wrapper<const std::vector<cv::Point>>>(lines).get();
  cv::polylines(yPlane, poly, true, color[0], thickness, cv::FILLED);
  std::vector<cv::Point> poly_half(poly);
  for (auto& point: poly_half) {
    point /= 2;
  }

  cv::polylines(uPlane, poly_half, true, color[1], thickness, cv::FILLED);
  cv::polylines(vPlane, poly_half, true, color[2], thickness, cv::FILLED);
}
}

int main() {
  // --- 1. Setup Frame and Parameters ---
  int width = 1920;
  int height = 1080;

  // Define the rectangle's position and size
  cv::Rect rectangle(100, 100, 200, 150);

  // Define the color in (Y, U, V) format. This is red.
  cv::Scalar yuvColor(76, 84, 255);

  // --- 2. Create a Dummy YUV420p Frame ---
  // The total size is height + height/2 rows, and width columns.
  cv::Mat yuvFrame = cv::Mat::zeros(height * 3 / 2, width, CV_8UC1);

  // For a black frame, Y is 0. U and V are 128 for neutral chroma.
  // We get the sub-mat for U and V and set them to 128.
  yuvFrame(cv::Rect(0, height, width, height / 2)).setTo(cv::Scalar(128));


  // --- 3. Draw the Rectangle ---
  drawOnYUV420p(yuvFrame, rectangle, yuvColor);
  // Draw a polygon
  std::vector<cv::Point> poly{{300, 300}, {550, 320}, {450, 360}, {400, 400}};
  drawOnYUV420p(yuvFrame, poly, yuvColor);
  std::cout << "Rectangle drawn on the YUV frame." << std::endl;


  // --- 4. Save the Result to a File ---
  std::ofstream outFile("output_frame.yuv", std::ios::binary);
  if (outFile.is_open()) {
    outFile.write(reinterpret_cast<const char*>(yuvFrame.data), yuvFrame.total() * yuvFrame.elemSize());
    outFile.close();
    std::cout << "Frame saved to 'output_frame.yuv'." << std::endl;
  } else {
    std::cerr << "Error: Could not open file for writing." << std::endl;
    return -1;
  }

  // --- 5. (Optional) Display the result for verification ---
  cv::Mat bgrFrame;
  cv::cvtColor(yuvFrame, bgrFrame, cv::COLOR_YUV2BGR_I420);

  cv::imwrite("test.jpg", bgrFrame);

  return 0;
}