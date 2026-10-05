# Nạp firmware qua WiFi (OTA)

Cách nạp lại firmware `device` cho board mà **không cần cáp USB**: board và máy build phải cùng
một mạng WiFi/LAN, và board phải đang chạy (đã kết nối WiFi từ trước).

## Dùng nhanh

```bash
tools/ota_upload.sh                 # dùng địa chỉ mặc định esp32.local
tools/ota_upload.sh 192.168.0.104   # hoặc chỉ thẳng IP, khi mDNS không phân giải được
```

Script tự `pio run -e device` để build, rồi `curl` đẩy file `.bin` lên board. Toàn bộ quá trình
mất khoảng 10–20 giây. Không cần giữ nút BOOT, không cần rút/cắm cáp.

## Chuẩn bị một lần

OTA chỉ chạy được khi hai điều kiện sau đã có, từ lúc cài dự án:

1. **`include/wifi_config.h` tồn tại và có khóa OTA thật.**
   File này không nằm trong git (xem `.gitignore`). Copy từ mẫu rồi điền:
   ```bash
   cp include/wifi_config.example.h include/wifi_config.h
   ```
   Sửa ba dòng trong đó:
   ```cpp
   static const char *WIFI_SSID = "ten-wifi-that";
   static const char *WIFI_PASSWORD = "mat-khau-that";
   static const char *OTA_KEY = "...một chuỗi dài, ngẫu nhiên, ít nhất 12 ký tự...";
   ```
   Firmware **từ chối mọi yêu cầu OTA** nếu `OTA_KEY` còn để nguyên giá trị mẫu `"change-me"`
   hoặc ngắn hơn 12 ký tự (`src/device.cpp`, trong handler `/ota`). Đây là khóa duy nhất bảo vệ
   tính năng này — ai biết khóa thì nạp được firmware bất kỳ vào board, nên không chia sẻ, không
   commit lên git.

2. **Board đã được nạp firmware `device` ít nhất một lần qua USB.** Lần đầu luôn phải qua USB
   (xem phần "Phân vùng và giới hạn" bên dưới) — lúc đó board mới có code biết lắng nghe
   `/ota`, có `wifi_config.h` biên dịch sẵn vào, và vào được WiFi để OTA lần sau.

## Board đang ở đâu?

Script mặc định dùng `esp32.local` (mDNS, hostname cấu hình trong code là `esp32`). Phần lớn máy
Linux/macOS và nhiều router phân giải được tên này ngay trên LAN. Nếu không được:

- Đọc log serial lúc board khởi động (cắm USB, mở monitor ở baud 115200), dòng:
  ```
  Ready: http://192.168.0.104/ | stored pages: N
  ```
  IP đó là địa chỉ hiện tại của board.
- Hoặc vào trang quản trị router, tìm thiết bị tên `esp32`.
- Rồi gọi: `tools/ota_upload.sh <IP đó>`.

IP có thể đổi mỗi lần board khởi động lại (DHCP), `esp32.local` thì không đổi — ưu tiên dùng tên
này khi mạng hỗ trợ.

## Chuyện gì xảy ra khi chạy script

1. **Trên máy build:** `pio run -e device` biên dịch ra `.pio/build/device/firmware.bin` (hiện
   khoảng 1,5 MB). Nếu build lỗi, script dừng ngay (set -e), không gửi gì lên board.
2. **Gửi lên board:** `curl` POST file đó dạng `multipart/form-data` tới
   `http://<host>/ota?key=<OTA_KEY>`.
3. **Trên board**, handler `/ota` trong `src/device.cpp` (đăng ký trong `setupWeb()`):
   - Kiểm tra khóa trong query string. Sai hoặc thiếu → dừng ngay, không ghi gì vào flash, trả
     về `403 Bad Request` kèm `OTA failed: bad key`.
   - Nếu đúng, bắt đầu ghi dữ liệu nhận được vào **phân vùng OTA còn trống** (xem phần Phân vùng
     bên dưới) — phân vùng đang chạy không bị đụng tới cho tới khi ghi xong toàn bộ và hợp lệ.
   - Màn hình board hiện "Updating" / "Đang cập nhật" kèm số KB đã nhận, cập nhật mỗi ~32 KB.
   - Nếu board đang ghi âm thì tự dừng ghi âm trước (`recStop()`), tránh ghi file dở.
   - Nhận xong, kiểm tra toàn vẹn (`Update.end(true)`). Nếu ổn: trả về
     `200 OK` kèm `OTA ok, N bytes, restarting`, đợi 300 ms rồi `ESP.restart()` — board khởi
     động lại bằng firmware mới.
   - Nếu lỗi khi ghi hoặc khi kiểm tra toàn vẹn: trả về `400 Bad Request` kèm
     `OTA failed: <mô tả lỗi>`, **không** khởi động lại, firmware cũ vẫn đang chạy bình thường.
4. Script in ra đúng nội dung board trả lời. Thấy `OTA ok, ... restarting` là xong.

## Gọi thẳng bằng curl (không qua script)

Hữu ích khi muốn OTA từ một máy khác, một pipeline CI, hay không có PlatformIO cài sẵn — chỉ cần
đúng file `.bin` đã build cho board này (board `adafruit_metro_esp32s3`, framework Arduino) và
khóa đúng:

```bash
curl -F "firmware=@firmware.bin" "http://esp32.local/ota?key=<OTA_KEY>"
```

Tên field trong multipart phải là `firmware` — đúng tên tham số script dùng, khớp với code nhận
upload trên board.

## Mã trả về

| HTTP | Nội dung | Ý nghĩa |
|---|---|---|
| 200 | `OTA ok, N bytes, restarting` | Thành công, board đang khởi động lại |
| 403 | `OTA failed: bad key` | Thiếu `?key=`, sai khóa, hoặc `OTA_KEY` trong firmware đang chạy vẫn là `"change-me"`/quá ngắn |
| 400 | `OTA failed: <chi tiết>` | Lỗi ghi flash (hết chỗ, dữ liệu hỏng giữa chừng, mất kết nối khi đang gửi...) |
| không phản hồi / curl timeout | — | Board không online, sai địa chỉ, không cùng mạng, hoặc mDNS không phân giải được `esp32.local` — dùng thẳng IP |

## Phân vùng và giới hạn

Board dùng bảng phân vùng `partitions-16MB-tinyuf2.csv`, có **hai phân vùng ứng dụng** độc lập:

```
ota_0   2048K   (0x10000)
ota_1   2048K   (0x210000)
```

OTA luôn ghi vào phân vùng *không* đang chạy, nên board không bao giờ tự xóa firmware đang hoạt
động trước khi có bản mới ghi xong và xác nhận toàn vẹn. Firmware hiện tại khoảng 1,5 MB, còn dư
nhiều so với hạn mức 2 MB mỗi phân vùng.

**Không có tính năng tự rollback.** Nếu bản mới ghi xong, xác nhận toàn vẹn OK, board khởi động
lại — nhưng bản đó có lỗi khiến nó treo hoặc không bao giờ mở được WiFi/web server, thì **không
còn cách nào OTA tiếp để sửa**, vì lúc đó board không nhận request nào nữa. Lúc này phải quay lại
nạp qua **USB** với bản firmware đã sửa lỗi:
```bash
pio run -e device -t upload
```
— đó là lý do mục Chuẩn bị một lần ở trên luôn cần một lần nạp USB đầu tiên.

## Những điều cần nhớ

- **Không rút nguồn hay tắt WiFi board giữa lúc đang OTA.** Mất kết nối giữa chừng làm phân vùng
  đang ghi dở dang; vì phân vùng đang chạy không hề bị đụng tới, board vẫn khởi động lại bình
  thường vào bản cũ, chỉ cần chạy lại OTA từ đầu.
- **Khóa đi dạng cleartext trên URL**, vì `/ota` chạy HTTP thường trong LAN, không mã hóa.
  An toàn ngang các endpoint khác của board (vốn chỉ dùng trong mạng nhà tin cậy), nhưng nếu đưa
  board ra mạng công cộng thì đừng để endpoint này lộ ra ngoài.
- Nếu đổi board khác (chip khác, bảng phân vùng khác) thì phải nạp USB trước, không OTA được
  ngay — firmware chạy sẵn trên board phải tự có code xử lý `/ota` thì mới nhận OTA tiếp theo
  được.
