// #include "Arduino.h"
#include <WiFi.h>
#include <WebSocketsServer.h> //websockets library by Markus Sattler
#include "ssid_stuff.h"

#include "fb_gfx.h"

#include <TensorFlowLite_ESP32.h>

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"

#include "p_det_model.h"
#include "model_settings.h"

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <esp_heap_caps.h>

#include "esp_camera.h"

#include "img_converters.h"
#include "Free_Fonts.h"

#include "soc/soc.h" // Disable brownout problems
#include "soc/rtc_cntl_reg.h" // Disable brownout problems

// Select camera model
// #define CAMERA_MODEL_WROVER_KIT // Has PSRAM
//#define CAMERA_MODEL_ESP_EYE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE  // Has PSRAM
#define CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM
// #define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM

#include "camera_pins.h"
#include "downsample.h"

const char *ssid = "WiFi";
const char *password = "******";

static bool done_inference = true;
static bool next_inference = false;
fb_data_t rfb; //for writing to the image.
unsigned long time_last_det = 0; //time since last detection.
unsigned long time_of_last_det; //time since last detection.

WebSocketsServer webSocket = WebSocketsServer(81);
WiFiServer server(80);
uint8_t cam_num;
bool connected = false;

#include "cam_stuff.h"

camera_fb_t * fb = NULL;
uint16_t *buffer;

size_t _jpg_buf_len = 0;
uint8_t * _jpg_buf = NULL;

QueueHandle_t do_inference = NULL;
QueueHandle_t next_image = NULL;

static float person_score_f ;
static float no_person_score_f;

unsigned long fps_last=1;
unsigned long fps_now=1;
unsigned long fps_next=10;

// Set your Static IP address
IPAddress local_IP(192, 168, 1, 196);
// Set your Gateway IP address
IPAddress gateway(192, 168, 1, 1);

IPAddress subnet(255, 255, 0, 0);
IPAddress primaryDNS(8, 8, 8, 8);   //optional
IPAddress secondaryDNS(8, 8, 4, 4); //optional

#define FACE_COLOR_WHITE 0x00FFFFFF
#define FACE_COLOR_BLACK 0x00000000
#define FACE_COLOR_RED 0x000000FF
#define FACE_COLOR_GREEN 0x0000FF00
#define FACE_COLOR_BLUE 0x00FF0000

//tflite stuff
tflite::ErrorReporter* error_reporter = nullptr;
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;


void liveCam(void *args){
  //capture a frame
  uint8_t nxt = 1;
  while (true){
  fps_last = millis();
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
      Serial.println("Frame buffer could not be acquired");
      continue;
  }

  
  if(done_inference){
    // Serial.println("done inference");
    done_inference = false;
    time_last_det =   millis() - time_of_last_det;
    time_of_last_det = millis();
    

    downsampleImage((uint16_t *) fb->buf, fb->width, fb->height);
    
    next_inference = true;

  }

  //display infering image in corner.
  // uint16_t * tmp = (uint16_t *) fb->buf;
  // for (int y = 0; y < DST_HEIGHT; y++) {
  //     for (int x = 0; x < DST_WIDTH; x++) {
  //       tmp[y*(fb->width) + x] = (uint16_t) dstImage[y*DST_WIDTH +x];

  //     }
  //   }

  //write confidence and time
  char buffer[5];
  char buff[5];

  rfb.width = fb->width;
  rfb.height = fb->height;
  rfb.data = fb->buf;
  rfb.bytes_per_pixel=2;
  rfb.format = FB_RGB565;
  // Serial.println(time_last_det);
  // Serial.println(person_score_f);
  unsigned int ps = (unsigned int) ((float) person_score_f*100.0);
  // Serial.println(ps);
  String str = String(time_last_det) + " : " + String(ps) + "%";
  String str2 = "fps (X10): " + String(fps_next);
  // Serial.println(str.c_str());
  fb_gfx_print(&rfb, 10, 10, FACE_COLOR_RED, str.c_str());
  fb_gfx_print(&rfb, 10, 50, FACE_COLOR_RED, str2.c_str());

  // fb_gfx_print(&rfb, (fb->width - (strlen(str.c_str()) * 14)) / 2, 10, FACE_COLOR_RED, str.c_str());
  // fb_gfx_print(&rfb, (fb->width - (strlen(str.c_str()) * 14)) / 2, 50, FACE_COLOR_RED, str2.c_str());

  bool jpeg_converted = frame2jpg(fb, 90, &_jpg_buf, &_jpg_buf_len);
  //replace this with your own function
  http_resp();
  webSocket.loop();
  if (connected == true){
    webSocket.broadcastBIN(_jpg_buf, _jpg_buf_len);
  }
  // Serial.print("fps last: ");
  // Serial.println(millis() - fps_last);

  fps_now = (unsigned long) (10000.0/((float)(millis() - fps_last)));
  fps_next= (unsigned long) (0.8*((float) fps_next) + 0.2*((float) fps_now)); //take exponentially weighted moving average to stabilize.
  // Serial.println(fps_next);
  // webSocket.sendBIN(num, fb->buf, fb->len);

  //return the frame buffer back to be reused
  if(fb){
      esp_camera_fb_return(fb);
      if(_jpg_buf){
      free(_jpg_buf);}
      fb = NULL;
      _jpg_buf = NULL;
    }

  // esp_camera_fb_return(fb);
  }
}

void tflite_infer(void *args){
  uint8_t nxt = 1;
  while(true){
    delay(200);
    Serial.println("task running");
    if(next_inference){
      Serial.println("next inference");
    int8_t * image_data = input->data.int8;
    
    next_inference = false;
    //do inference
    for (int i = 0; i < kNumRows; i++) {
      for (int j = 0; j < kNumCols; j++) {
        uint16_t pixel = ((uint16_t *) (dstImage))[i * kNumCols + j];

        // for inference
        uint8_t hb = pixel & 0xFF;
        uint8_t lb = pixel >> 8;
        uint8_t r = (lb & 0x1F) << 3;
        uint8_t g = ((hb & 0x07) << 5) | ((lb & 0xE0) >> 3);
        uint8_t b = (hb & 0xF8);

        /**
        * Gamma corected rgb to greyscale formula: Y = 0.299R + 0.587G + 0.114B
        * for effiency we use some tricks on this + quantize to [-128, 127]
        */
        int8_t grey_pixel = ((305 * r + 600 * g + 119 * b) >> 10) - 128;

        image_data[i * kNumCols + j] = grey_pixel;

       
      }
    }
    if (kTfLiteOk != interpreter->Invoke()) {
      TF_LITE_REPORT_ERROR(error_reporter, "Invoke failed.");
    }
    

    TfLiteTensor* output = interpreter->output(0);
    

    // Process the inference results.
    int8_t person_score = output->data.uint8[kPersonIndex];
    
    int8_t no_person_score = output->data.uint8[kNotAPersonIndex];
    
    person_score_f =
        (person_score - output->params.zero_point) * output->params.scale;
    no_person_score_f =
        (no_person_score - output->params.zero_point) * output->params.scale;

    // Serial.print("person score: "); Serial.println(person_score_f);
    // Serial.print("no person score: "); Serial.println(no_person_score_f);
    done_inference = true;
    // xQueueSend(do_inference,&done_inference,portMAX_DELAY);
    
  }
  
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {

    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Disconnected!\n", num);
            break;
        case WStype_CONNECTED:
            cam_num = num;
            connected = true;
            break;
        case WStype_TEXT:
        case WStype_BIN:
        case WStype_ERROR:      
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
            break;
    }
}

// In order to use optimized tensorflow lite kernels, a signed int8_t quantized
// model is preferred over the legacy unsigned model format. This means that
// throughout this project, input images must be converted from unisgned to
// signed format. The easiest and quickest way to convert from unsigned to
// signed 8-bit integers is to subtract 128 from the unsigned value to get a
// signed value.

#ifdef CONFIG_IDF_TARGET_ESP32S3
constexpr int scratchBufSize = 39 * 1024;
#else
constexpr int scratchBufSize = 0;
#endif
// An area of memory to use for input, output, and intermediate arrays.
constexpr int kTensorArenaSize = 81 * 1024 + scratchBufSize;
static uint8_t *tensor_arena;//[kTensorArenaSize]; // Maybe we should move this to external


void init_camera(){
  Serial.println("in config");
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;//PIXFORMAT_GRAYSCALE;//PIXFORMAT_JPEG;//PIXFORMAT_RGB565;// 
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.frame_size = FRAMESIZE_QVGA;//FRAMESIZE_96X96;//FRAMESIZE_QVGA;//FRAMESIZE_96X96;//
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  
  config.jpeg_quality = 12;
  config.fb_count = 1;
  
  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  // if(psramFound()){
  //   // config.frame_size = FRAMESIZE_QVGA;//FRAMESIZE_96X96;//
  //   config.jpeg_quality = 12;
  //   config.fb_count = 2;
  // } else {
  //   // config.frame_size = FRAMESIZE_QVGA;//FRAMESIZE_96X96;//
  //   config.jpeg_quality = 12;
  //   config.fb_count = 2;
  // }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    delay(1000);
    ESP.restart();
  }
#if defined(CAMERA_MODEL_M5STACK_WIDE)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif
}


void setup() {
  
  // put your setup code here, to run once:
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);//disable brownout detector

  Serial.begin(115200);
  init_camera();
  // buffer = (uint16_t *) malloc(240*320*2);

  
  dstImage = (uint16_t *) malloc(DST_WIDTH * DST_HEIGHT*2);
  delay(200);
  static tflite::MicroErrorReporter micro_error_reporter;
  error_reporter = &micro_error_reporter;

  model = tflite::GetModel(g_person_detect_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    TF_LITE_REPORT_ERROR(error_reporter,
                         "Model provided is schema version %d not equal "
                         "to supported version %d.",
                         model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }
  if (tensor_arena == NULL) {
    tensor_arena = (uint8_t *) heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // tensor_arena = (uint8_t *) ps_calloc(kTensorArenaSize,1);// MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  
  }
  if (tensor_arena == NULL) {
    printf("Couldn't allocate memory of %d bytes\n", kTensorArenaSize);
    return;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  //
  /*/
  tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, kTensorArenaSize, error_reporter);
      *///
  // NOLINTNEXTLINE(runtime-global-variables)
  //
  static tflite::MicroMutableOpResolver<5> micro_op_resolver;
  micro_op_resolver.AddAveragePool2D();
  micro_op_resolver.AddConv2D();
  micro_op_resolver.AddDepthwiseConv2D();
  micro_op_resolver.AddReshape();
  micro_op_resolver.AddSoftmax();
  

  // Build an interpreter to run the model with.
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
      ////
  
  interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    TF_LITE_REPORT_ERROR(error_reporter, "AllocateTensors() failed");
    return;
  }

  // Get information about the memory area to use for the model's input.
  input = interpreter->input(0);

  //create queus
  do_inference = xQueueCreate(1, sizeof(uint8_t *));
  next_image = xQueueCreate(1, sizeof(uint8_t *));
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }
  WiFi.begin(ssid, password);
  Serial.println("");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  String IP = WiFi.localIP().toString();
  Serial.print("IP address: " + IP);
  index_html.replace("server_ip", IP);
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

   xTaskCreatePinnedToCore(
       liveCam, /* Function to implement the task */
       "lcam",    /* Name of the task */
       1024*4,            /* Stack size in words */
       NULL,            /* Task input parameter */
       2,               /* Priority of the task */
       NULL,     /* Task handle. */
       1);   

   xTaskCreatePinnedToCore(
       tflite_infer, /* Function to implement the task */
       "tflite",    /* Name of the task */
       1024*10,            /* Stack size in words */
       NULL,            /* Task input parameter */
       2,               /* Priority of the task */
       NULL,     /* Task handle. */
       1);   
  
  time_of_last_det = millis();

}

void http_resp(){
  WiFiClient client = server.available();
  if (client.connected() && client.available()) {                   
    client.flush();          
    client.print(index_html);
    client.stop();
  }
}


void loop() {
  // put your main code here, to run repeatedly:
  vTaskDelete(NULL);

}
