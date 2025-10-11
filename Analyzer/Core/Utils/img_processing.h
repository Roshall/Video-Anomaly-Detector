#ifndef ANALYZER_CALCUIOU_H
#define ANALYZER_CALCUIOU_H

#include <vector>
#include <variant>
#include <opencv2/opencv.hpp>

namespace avs_analyzer {

/*
调用案例

double p[8] = { 0, 0, 0, 80, 300, 80, 300, 0 }; // x1,y1,x2,y2,x3,y3,x4,y4
double q[8] = { 0, 40, 0, 120, 300, 120, 300, 40 };

std::vector<double> P(p, p + 8);
std::vector<double> Q(q, q + 8);
double iou = CalcuPolygonIOU(P, Q);
*/
double CalcuPolygonIOU(std::vector<double> p, std::vector<double> q);
using RectOrPoly = std::variant<std::reference_wrapper<const cv::Rect>, std::reference_wrapper<const std::vector<cv::Point>>>;
void drawOnYUV420p(cv::Mat& yuvFrame, RectOrPoly lines, const cv::Scalar& color, bool rectangle=true, int thickness=5);
}  // namespace avs_analyzer
#endif  // ANALYZER_CALCUIOU_H