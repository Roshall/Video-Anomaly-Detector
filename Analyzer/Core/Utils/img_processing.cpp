#include "img_processing.h"

#include <algorithm>
#include <cmath>

using namespace std;
namespace avs_analyzer {
/**
 * @brief Draws a rectangle/polygon on a YUV420p frame.
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
constexpr int  maxn  = 51;
constexpr double eps = 1E-8;
int sig(double d) { return (d > eps) - (d < -eps); }
struct Point {
  double x{}, y{};
  Point() = default;
  Point(double x, double y) : x(x), y(y) {}
  bool operator==(const Point& p) const {
    return sig(x - p.x) == 0 && sig(y - p.y) == 0;
  }
};
double cross(Point o, Point a, Point b) {  // 叉积
  return (a.x - o.x) * (b.y - o.y) - (b.x - o.x) * (a.y - o.y);
}
double area(Point* ps, int n) {
  ps[n] = ps[0];
  double res = 0;
  for (int i = 0; i < n; i++) {
    res += ps[i].x * ps[i + 1].y - ps[i].y * ps[i + 1].x;
  }
  return res / 2.0;
}
int lineCross(Point a, Point b, Point c, Point d, Point& p) {
  double s1, s2;
  s1 = cross(a, b, c);
  s2 = cross(a, b, d);
  if (sig(s1) == 0 && sig(s2) == 0) return 2;
  if (sig(s2 - s1) == 0) return 0;
  p.x = (c.x * s2 - d.x * s1) / (s2 - s1);
  p.y = (c.y * s2 - d.y * s1) / (s2 - s1);
  return 1;
}
// 多边形切割
// 用直线ab切割多边形p，切割后的在向量(a,b)的左侧，并原地保存切割结果
// 如果退化为一个点，也会返回去,此时n为1
// void polygon_cut(Point*p,int&n,Point a,Point b){
//     static Point pp[maxn];
//     int m=0;p[n]=p[0];
//     for(int i=0;i<n;i++){
//         if(sig(cross(a,b,p[i]))>0) pp[m++]=p[i];
//         if(sig(cross(a,b,p[i]))!=sig(cross(a,b,p[i+1])))
//             lineCross(a,b,p[i],p[i+1],pp[m++]);
//     }
//     n=0;
//     for(int i=0;i<m;i++)
//         if(!i||!(pp[i]==pp[i-1]))
//             p[n++]=pp[i];
//     while(n>1&&p[n-1]==p[0])n--;
// }
void polygon_cut(Point* p, int& n, Point a, Point b, Point* pp) {
  //    static Point pp[maxn];
  int m = 0;
  p[n] = p[0];
  for (int i = 0; i < n; i++) {
    if (sig(cross(a, b, p[i])) > 0) pp[m++] = p[i];
    if (sig(cross(a, b, p[i])) != sig(cross(a, b, p[i + 1])))
      lineCross(a, b, p[i], p[i + 1], pp[m++]);
  }
  n = 0;
  for (int i = 0; i < m; i++)
    if (!i || !(pp[i] == pp[i - 1])) p[n++] = pp[i];
  while (n > 1 && p[n - 1] == p[0]) n--;
}
//---------------华丽的分隔线-----------------//
// 返回三角形oab和三角形ocd的有向交面积,o是原点//
double intersectArea(Point a, Point b, Point c, Point d) {
  Point o(0, 0);
  int s1 = sig(cross(o, a, b));
  int s2 = sig(cross(o, c, d));
  if (s1 == 0 || s2 == 0) return 0.0;  // 退化，面积为0
  if (s1 == -1) swap(a, b);
  if (s2 == -1) swap(c, d);
  Point p[10] = {o, a, b};
  int n = 3;
  Point pp[maxn];
  polygon_cut(p, n, o, c, pp);
  polygon_cut(p, n, c, d, pp);
  polygon_cut(p, n, d, o, pp);
  double res = fabs(area(p, n));
  if (s1 * s2 == -1) res = -res;
  return res;
}
// 求两多边形的交面积
double intersectArea(Point* ps1, int n1, Point* ps2, int n2) {
  if (area(ps1, n1) < 0) reverse(ps1, ps1 + n1);
  if (area(ps2, n2) < 0) reverse(ps2, ps2 + n2);
  ps1[n1] = ps1[0];
  ps2[n2] = ps2[0];
  double res = 0;
  for (int i = 0; i < n1; i++) {
    for (int j = 0; j < n2; j++) {
      res += intersectArea(ps1[i], ps1[i + 1], ps2[j], ps2[j + 1]);
    }
  }
  return res;  // assumeresispositive!
}

double CalcuPolygonIOU(std::vector<double> p, std::vector<double> q) {
  Point ps1[maxn], ps2[maxn];
  int n1 = 4;
  int n2 = 4;
  for (int i = 0; i < 4; i++) {
    ps1[i].x = p[i * 2];
    ps1[i].y = p[i * 2 + 1];

    ps2[i].x = q[i * 2];
    ps2[i].y = q[i * 2 + 1];
  }
  double area1 = fabs(area(ps1, n1));
  double area2 = fabs(area(ps2, n2));

  double inter_area = intersectArea(ps1, n1, ps2, n2);
  // double union_area = area1 + area2 - inter_area;
  // double iou = inter_area / union_area;
  double iou = inter_area / area2;

  //    cout << "inter_area:" << inter_area << endl;
  //    cout << "union_area:" << union_area << endl;
  //    cout << "iou:" << iou << endl;

  return iou;
}

}  // namespace avs_analyzer