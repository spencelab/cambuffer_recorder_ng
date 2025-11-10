/* on mac

g++ extract_ts2.cpp -std=c++17 -O2 -o extract_ts2 \
    -I$CONDA_PREFIX/include/opencv4 -I$CONDA_PREFIX/include \
    -L$CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib \
    -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
    -lopencv_videoio -lopencv_highgui -ltesseract

DYLD_FALLBACK_LIBRARY_PATH=$CONDA_PREFIX/lib ./extract_ts2 out_half.mp4 timestamps.csv
*/

#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace cv;
using namespace std;

int main(int argc, char** argv) {
    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " input.mp4 output.csv\n";
        return 1;
    }

    string input_file = argv[1];
    string output_csv = argv[2];

    VideoCapture cap(input_file);
    if (!cap.isOpened()) {
        cerr << "Error opening video file: " << input_file << "\n";
        return 1;
    }

    int w = (int)cap.get(CAP_PROP_FRAME_WIDTH);
    int h = (int)cap.get(CAP_PROP_FRAME_HEIGHT);
    cout << "Video size: " << w << " x " << h << endl;

    // --- ROI covering upper-right timestamp area ---
    double roi_x_frac = 0.80;  // start 65% from left
    double roi_y_frac = 0.00;  // start 2% from top
    double roi_w_frac = 0.20;  // rightmost 1/3
    double roi_h_frac = 0.10;  // top 10%

    Rect roi(
        int(w * roi_x_frac),
        int(h * roi_y_frac),
        int(w * roi_w_frac),
        int(h * roi_h_frac)
    );

    cout << "Using ROI: x=" << roi.x << ", y=" << roi.y
         << ", w=" << roi.width << ", h=" << roi.height << endl;

    tesseract::TessBaseAPI tess;
    if (tess.Init(NULL, "eng", tesseract::OEM_LSTM_ONLY)) {
        cerr << "Could not initialize tesseract.\n";
        return 1;
    }
    tess.SetPageSegMode(tesseract::PSM_SINGLE_LINE);
    tess.SetVariable("tessedit_char_whitelist", "0123456789.");
    tess.SetVariable("load_system_dawg", "F");
    tess.SetVariable("load_freq_dawg", "F");
    tess.SetVariable("classify_bln_numeric_mode", "1");

    ofstream csv(output_csv);
    csv << "frame,unix_time\n";

    int frame_index = 0;
    bool debug_saved = false;
    Mat frame;

    while (true) {
        if (!cap.read(frame)) break;

        Mat roi_frame = frame(roi);

        // --- Save ROI + binarized image on first frame for inspection ---
        if (!debug_saved) {
            imwrite("ocr_roi_debug.png", roi_frame);

            vector<Mat> chans;
            split(roi_frame, chans);
            Mat red = chans[2];

            //GaussianBlur(red, red, Size(3,3), 0);
            Mat bin;
            threshold(red, bin, 130, 255, THRESH_BINARY);
            Mat kernel = getStructuringElement(MORPH_RECT, Size(2,2));
            dilate(bin, bin, kernel);
            imwrite("ocr_bin_debug.png", bin);

            //imwrite("ocr_bin_debug.png", red);
            Mat debug = frame.clone();
            rectangle(debug, roi, Scalar(0,0,255), 2);
            imwrite("ocr_debug_roi.png", debug);

            cout << "Saved debug images:\n"
                 << "  ocr_roi_debug.png (ROI crop)\n"
                 << "  ocr_bin_debug.png (red channel threshold)\n"
                 << "  ocr_debug_roi.png (full frame with red box)\n";

            debug_saved = true;
        }

        // --- Red-channel extraction + threshold for OCR ---
        vector<Mat> chans;
        split(roi_frame, chans);
        Mat red = chans[2];

        //GaussianBlur(red, red, Size(3,3), 0);
        Mat bin;
        threshold(red, bin, 130, 255, THRESH_BINARY);
        Mat kernel = getStructuringElement(MORPH_RECT, Size(2,2));
        dilate(bin, bin, kernel);

        //GaussianBlur(red, red, Size(3,3), 0);

        //Mat bin;
        //threshold(red, bin, 180, 255, THRESH_BINARY);
        //Mat kernel = getStructuringElement(MORPH_RECT, Size(2,2));
        //dilate(bin, bin, kernel);

        tess.SetImage(bin.data, bin.cols, bin.rows, 1, bin.step);
        string text = tess.GetUTF8Text();

        text.erase(remove(text.begin(), text.end(), '\n'), text.end());
        text.erase(remove(text.begin(), text.end(), ' '), text.end());

        double ts = -1.0;
        try { ts = stod(text); } catch (...) {}

        csv << frame_index << "," << std::fixed << std::setprecision(6) << ts << "\n";
        frame_index++;
    }

    cap.release();
    csv.close();
    cout << "Done. CSV written to " << output_csv << endl;
    return 0;
}
