#include <bitset>
#include "mbed.h"
#include "EasyAttach_CameraAndLCD.h"
#include "SdUsbConnect.h"
#include "JPEG_Converter.h"
#include "dcache-control.h"
#include "opencv2/opencv.hpp"
#include "DisplayApp.h"
#include "tinypcl.hpp"

// 筐体に依存するパラメーター
#define CAMERA_DISTANCE 115     // 原点(ステッピングモーター回転軸)からカメラの距離(mm)
#define CAMERA_OFFSET  3        // カメラ高さの調整(mm)

// ステッピングモーターの出力ピン(ステッピングモータードライバとしてA4988を利用)
DigitalOut a4988_step(D8);
DigitalOut a4988_dir(D9);

// ボタンの入力ピン
DigitalIn   button0(D4);
DigitalIn   button1(D6);

// 状態表示LEDの出力ピン
DigitalOut  led_working(D7);    // 処理中
DigitalOut  led_ready(D5);      // 背景画像取得完了

// デバッグ用（カメラが準備完了になった時にLED1を点灯）
DigitalOut  led1(LED1);

// ステッピングモーター関連のパラメーター
#define STEPPER_DIRECTION 1     // ステッピングモーターの回転方向(0 または 1)
#define STEPPER_WAIT    0.004   // wait
#define STEPPER_STEPS   800     // 1周に必要なステップ数（Quarter step）
#define STEPPER_STEP    20      // 1回のステップ数

// カメラ内部パラメーター（OpenCVのカメラキャリブレーションが必要）
#define CAMERA_CENTER_U 321     // 画像中心(横方向)
#define CAMERA_CENTER_V 244     // 画像中心（縦方向）
#define CAMERA_FX 365.202395    // カメラ焦点距離(fx)
#define CAMERA_FY 365.519979    // カメラ焦点距離(fy)

// 復元関連のパラメーター
#define SILHOUETTE_THRESH_BINARY 30     // 二値化する際のしきい値

// 復元関連のデータ
PointCloud point_cloud;      // 仮想物体（復元する点群）

cv::Mat img_background;      // 背景画像
bool has_background = false; // 背景画像を取得済かどうかを管理するフラグ
int reconst_count = 1;       // 復元結果ファイルのインデックス
char file_name[32];          // 出力ファイル名

#define MOUNT_NAME             "storage"

#define DBG_PCMONITOR (1)
#if (DBG_PCMONITOR == 1)
/* For viewing image on PC */
static DisplayApp  display_app;
#endif


/* Video input and LCD layer 0 output */
#define VIDEO_FORMAT           (DisplayBase::VIDEO_FORMAT_YCBCR422)
#define GRAPHICS_FORMAT        (DisplayBase::GRAPHICS_FORMAT_YCBCR422)
#define WR_RD_WRSWA            (DisplayBase::WR_RD_WRSWA_32_16BIT)
#define DATA_SIZE_PER_PIC      (2u)

/*! Frame buffer stride: Frame buffer stride should be set to a multiple of 32 or 128
    in accordance with the frame buffer burst transfer mode. */
#define VIDEO_PIXEL_HW         (640u)  /* VGA */
#define VIDEO_PIXEL_VW         (480u)  /* VGA */

#define FRAME_BUFFER_STRIDE    (((VIDEO_PIXEL_HW * DATA_SIZE_PER_PIC) + 31u) & ~31u)
#define FRAME_BUFFER_HEIGHT    (VIDEO_PIXEL_VW)

#if defined(__ICCARM__)
#pragma data_alignment=32
static uint8_t user_frame_buffer0[FRAME_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]@ ".mirrorram";
#pragma data_alignment=4
#else
static uint8_t user_frame_buffer0[FRAME_BUFFER_STRIDE * FRAME_BUFFER_HEIGHT]__attribute((section("NC_BSS"),aligned(32)));
#endif
static int file_name_index = 1;
/* jpeg convert */
static JPEG_Converter Jcu;
#if defined(__ICCARM__)
#pragma data_alignment=32
static uint8_t JpegBuffer[1024 * 63];
#else
static uint8_t JpegBuffer[1024 * 63]__attribute((aligned(32)));
#endif

DisplayBase Display;

// 背景画像の取得
void get_background_image(void) {
    // Transform buffer into OpenCV Mat
    cv::Mat img_yuv(VIDEO_PIXEL_VW, VIDEO_PIXEL_HW, CV_8UC2, user_frame_buffer0);

    // Convert from YUV422 to grayscale
    cv::cvtColor(img_yuv, img_background, CV_YUV2GRAY_YUY2);

    // set flag
    has_background = true;
    led_ready = 1;
}

// 3次元座標から2次元座標への変換
int projection(double rad, double Xw, double Yw,double Zw, int &u, int &v)
{
    // （仮想物体を）ワールド座標の原点を中心にY軸周りに回転
    double Xc= cos(rad)*Xw + sin(rad)*Zw;
    double Yc= Yw;
    double Zc=-sin(rad)*Xw + cos(rad)*Zw;

    // ワールド座標からカメラ座標への変換（Z軸方向の移動のみ）
    Yc+=CAMERA_OFFSET;
    Zc-=CAMERA_DISTANCE;
  
    // 画像座標へ変換
    u= CAMERA_CENTER_U - (int)((Xc/Zc)*(CAMERA_FX));
    v= CAMERA_CENTER_V - (int)((Yc/Zc)*(CAMERA_FY));

    return (u>0 && u<VIDEO_PIXEL_HW && v>0 && v<VIDEO_PIXEL_VW);
}

// 輪郭画像からの立体形状復元
// 3d reconstruction from silhouette
void reconst(double rad) {
    // Transform buffer into OpenCV Mat
    cv::Mat img_yuv(VIDEO_PIXEL_VW, VIDEO_PIXEL_HW, CV_8UC2, user_frame_buffer0);

    // Convert from YUV422 to grayscale
    cv::Mat img_silhouette;
    cv::cvtColor(img_yuv, img_silhouette, CV_YUV2GRAY_YUY2);

    // 背景画像の除去と輪郭画像の取得
    // Background subtraction and get silhouette
    cv::absdiff(img_silhouette, img_background, img_silhouette);
    cv::threshold(img_silhouette, img_silhouette, SILHOUETTE_THRESH_BINARY, 255, cv::THRESH_BINARY);

    // 輪郭画像の出力（デバッグ用）
    // sprintf(file_name, "/"MOUNT_NAME"/img_%d.bmp", file_name_index);
    // cv::imwrite(file_name, img_silhouette);
    // printf("Saved file %s\r\n", file_name);

    // 輪郭画像による仮想物体の型抜き - Shape from silhouette
    // このプログラムでは、仮想物体の形状を点群(point cloud data)で表現している。
    // point_cloud_data(x,y,z) = 0の場合、そこには物体がないことを意味する。
    // point_cloud_data(x,y,z) = 1の場合、そこには物体がある（可能性がある）ことを意味する。
    // 型抜きとは、仮想物体の復元対象点ごとに、輪郭画像内外を判定し、輪郭画像外であれば除去、輪郭画像内であれば保持を繰り返すこと。
    // このプログラムでは、原点を中心にxyzそれぞれ-50mm ~ +50mmの範囲を1mm単位で復元する。
    double xx,yy,zz;    // 復元対象の点の座標値(x,y,z)
    int u,v;            // 復元対象の点の、カメラ画像内での座標値(x,y)
    int pcd_index=0;    // 仮想物体の復元対象点

    zz = (-point_cloud.POINTS / 2) * point_cloud.SCALE;
    for (int z=0; z<point_cloud.POINTS; z++, zz += point_cloud.SCALE) {

        yy = (-point_cloud.POINTS / 2) * point_cloud.SCALE;
        for (int y=0; y<point_cloud.POINTS; y++, yy += point_cloud.SCALE) {

            xx = (-point_cloud.POINTS / 2) * point_cloud.SCALE;
            for (int x=0; x<point_cloud.POINTS; x++, xx += point_cloud.SCALE, pcd_index++) {
                if (point_cloud.get(pcd_index) == 1) {
                    
                    // 復元対象の点がカメラ画像内ではどこにあるかを計算する
                    if (projection(rad, xx, yy, zz, u, v)) {
                        // カメラ画像内のため、輪郭画像と比較する
                        if (img_silhouette.at<unsigned char>(v, u)) {
                            // 復元対象の点は、輪郭画像内（白色）のため、そのまま
                        }
                        else {
                            // 復元対象の点は、輪郭画像外（黒色）のため、除去
                            point_cloud.set(pcd_index, 0);
                        }
                    } else {
                        // カメラ画像外のためクリアする
                        point_cloud.set(pcd_index, 0);
                    }
                }
            }
        }
    }
}

// ステッピングモーターの回転(ステッピングモータードライバとしてA4988を利用)
void rotate(int steps) {
    a4988_dir = STEPPER_DIRECTION;
    for (int i=0;i<steps;i++) {
        a4988_step = 1;
        wait(STEPPER_WAIT);
        a4988_step = 0;
        wait(STEPPER_WAIT);
    }
}

static void save_image_jpg(void) {
    size_t jcu_encode_size;
    JPEG_Converter::bitmap_buff_info_t bitmap_buff_info;
    JPEG_Converter::encode_options_t   encode_options;

    bitmap_buff_info.width              = VIDEO_PIXEL_HW;
    bitmap_buff_info.height             = VIDEO_PIXEL_VW;
    bitmap_buff_info.format             = JPEG_Converter::WR_RD_YCbCr422;
    bitmap_buff_info.buffer_address     = (void *)user_frame_buffer0;

    encode_options.encode_buff_size     = sizeof(JpegBuffer);
    encode_options.p_EncodeCallBackFunc = NULL;
    encode_options.input_swapsetting    = JPEG_Converter::WR_RD_WRSWA_32_16_8BIT;

    jcu_encode_size = 0;
    dcache_invalid(JpegBuffer, sizeof(JpegBuffer));
    if (Jcu.encode(&bitmap_buff_info, JpegBuffer, &jcu_encode_size, &encode_options) != JPEG_Converter::JPEG_CONV_OK) {
        jcu_encode_size = 0;
    }

    sprintf(file_name, "/"MOUNT_NAME"/img_%d.jpg", file_name_index);
    FILE * fp = fopen(file_name, "w");
    fwrite(JpegBuffer, sizeof(char), (int)jcu_encode_size, fp);
    fclose(fp);
    printf("Saved file %s\r\n", file_name);
}

static void Start_Video_Camera(void) {
    // Video capture setting (progressive form fixed)
    Display.Video_Write_Setting(
        DisplayBase::VIDEO_INPUT_CHANNEL_0,
        DisplayBase::COL_SYS_NTSC_358,
        (void *)user_frame_buffer0,
        FRAME_BUFFER_STRIDE,
        VIDEO_FORMAT,
        WR_RD_WRSWA,
        VIDEO_PIXEL_VW,
        VIDEO_PIXEL_HW
    );
    EasyAttach_CameraStart(Display, DisplayBase::VIDEO_INPUT_CHANNEL_0);
}


size_t encode_jpeg(uint8_t* buf, int len, int width, int height, uint8_t* inbuf) {
    size_t encode_size;
    JPEG_Converter::bitmap_buff_info_t bitmap_buff_info;
    JPEG_Converter::encode_options_t encode_options;
    bitmap_buff_info.width = width;
    bitmap_buff_info.height = height;
    bitmap_buff_info.format = JPEG_Converter::WR_RD_YCbCr422;
    bitmap_buff_info.buffer_address = (void *) inbuf;
    encode_options.encode_buff_size = len;
    encode_options.p_EncodeCallBackFunc = NULL;
    encode_options.input_swapsetting = JPEG_Converter::WR_RD_WRSWA_32_16_8BIT;

    encode_size = 0;
    dcache_invalid(buf, len);
    if (Jcu.encode(&bitmap_buff_info, buf, &encode_size, &encode_options)
            != JPEG_Converter::JPEG_CONV_OK) {
        encode_size = 0;
    }

    return encode_size;
}

size_t create_jpeg(){
    return encode_jpeg(JpegBuffer, sizeof(JpegBuffer), VIDEO_PIXEL_HW, VIDEO_PIXEL_VW, user_frame_buffer0);
}

uint8_t* get_jpeg_adr(){
    return JpegBuffer;
}

int main() {
    // Initialize the background to black
    for (int i = 0; i < sizeof(user_frame_buffer0); i += 2) {
        user_frame_buffer0[i + 0] = 0x10;
        user_frame_buffer0[i + 1] = 0x80;
    }

    // Camera
    EasyAttach_Init(Display);
    Start_Video_Camera();
    led1 = 1;

    // SD & USB
    SdUsbConnect storage(MOUNT_NAME);

    // Stepping motor
    a4988_dir = 0;
    a4988_step = 0;

    // clear Point cloud data
    point_cloud.clear();

    while (1) {
        storage.wait_connect();

        if (button0 == 0) {
            // 背景画像の取得
            led_working = 1;
            get_background_image(); // get background image
            save_image_jpg(); // save as jpeg
            file_name_index++;
            led_working = 0;

            wait_ms(100);
        }
        if (button1 == 0 && has_background) {
            // Shape from silhouette アルゴリズムによる立体形状復元
            // テーブルを回転させながら輪郭画像の取得と立体形状復元を繰り返す
            for (int i=0;i<(STEPPER_STEPS/STEPPER_STEP);i++) {
#if (DBG_PCMONITOR == 1)
                // プレビュー画像の送信
                size_t jpeg_size = create_jpeg();
                display_app.SendJpeg(get_jpeg_adr(), jpeg_size);
#endif

                // 輪郭画像の取得と立体形状復元
                led_working = 1;
                double rad = (double)(2*3.14)*((double)i/(STEPPER_STEPS/STEPPER_STEP));
                reconst(rad);
                save_image_jpg(); // save as jpeg
                led_working = 0;
                file_name_index++;

                // テーブルの回転
                rotate(STEPPER_STEP);
            }

            // 復元した立体形状データの修正
            point_cloud.remove_edge();

            // 復元した立体形状データの出力
            cout << "writting..." << endl;
            led_working = 1;

            sprintf(file_name, "/"MOUNT_NAME"/result_%d.xyz", reconst_count);
            point_cloud.save_as_xyz(file_name);
            sprintf(file_name, "/"MOUNT_NAME"/result_%d.stl", reconst_count);
            point_cloud.save_as_stl(file_name);
            // sprintf(file_name, "/"MOUNT_NAME"/result_%d.ply", reconst_count);
            // point_cloud.save_as_ply(file_name);
            reconst_count++;

            led_working = 0;
            cout << "finish" << endl;
            point_cloud.clear();
        }

#if (DBG_PCMONITOR == 1)
        size_t jpeg_size = create_jpeg();
        display_app.SendJpeg(get_jpeg_adr(), jpeg_size);
#endif

    }
}
