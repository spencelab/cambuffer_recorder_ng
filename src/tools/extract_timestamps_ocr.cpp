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

    // Get frame dimensions
    int w = (int)cap.get(CAP_PROP_FRAME_WIDTH);
    int h = (int)cap.get(CAP_PROP_FRAME_HEIGHT);
    cout << "Video size: " << w << " x " << h << endl;

    // --- Define ROI (upper-right corner, adjustable) ---
    // These percentages should roughly cover your red timestamp area.
    double roi_x_frac = 0.80;  // start 65% from left
    double roi_y_frac = 0.00;  // start 2% from top
    double roi_w_frac = 0.20;  // width covers rightmost 1/3
    double roi_h_frac = 0.10;  // height covers top 10%

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

    ofstream csv(output_csv);
    csv << "frame,unix_time\n";

    int frame_index = 0;
    Mat frame, roi_img, gray;
    bool debug_saved = false;

    while (true) {
        if (!cap.read(frame)) break;

        Mat roi_frame = frame(roi);

        // Save the first frame with red ROI box for visual check
        if (!debug_saved) {
            Mat debug = frame.clone();
            rectangle(debug, roi, Scalar(0, 0, 255), 2);
            imwrite("ocr_debug_roi.png", debug);
            cout << "Saved debug image: ocr_debug_roi.png\n";
            debug_saved = true;
        }

        // Extract only the red channel (since overlay text is pure red)
        vector<Mat> chans;
        split(roi_frame, chans);
        Mat red = chans[2];  // B,G,R → use R

        // Optional: reduce compression noise
        GaussianBlur(red, red, Size(3,3), 0);

        // Threshold to isolate red digits
        Mat bin;
        threshold(red, bin, 180, 255, THRESH_BINARY);  // adjust 160–200 depending on brightness

        // Optional: dilate slightly to join thin strokes
        Mat kernel = getStructuringElement(MORPH_RECT, Size(2,2));
        dilate(bin, bin, kernel);


        tess.SetImage(bin.data, bin.cols, bin.rows, 1, bin.step);
        string text = tess.GetUTF8Text();

        // Clean up OCR result
        text.erase(remove(text.begin(), text.end(), '\n'), text.end());
        text.erase(remove(text.begin(), text.end(), ' '), text.end());

        double ts = 0.0;
        try {
            ts = stod(text);
        } catch (...) {
            ts = -1.0; // failed parse
        }

        csv << frame_index << "," << ts << "\n";
        frame_index++;
    }

    cap.release();
    csv.close();
    cout << "Done. CSV written to " << output_csv << endl;
    return 0;
}
