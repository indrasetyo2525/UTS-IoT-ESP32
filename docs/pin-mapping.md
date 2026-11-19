# Pin Mapping

Sesuai konstanta di `src/main.cpp`. Jika kamu mengubah pin di kode, ubah tabel ini juga.

| Perangkat | Pin perangkat | Pin ESP32-S3 | Catatan |
|---|---|---|---|
| HC-SR04 | VCC | 5V | |
| HC-SR04 | GND | GND | |
| HC-SR04 | TRIG | GPIO 5 | Output 3,3V sudah cukup memicu sensor |
| HC-SR04 | ECHO | GPIO 6 | **Wajib lewat pembagi tegangan** |
| LCD I2C | VCC | 5V | |
| LCD I2C | GND | GND | |
| LCD I2C | SDA | GPIO 8 | Pin I2C bawaan board DevKitC |
| LCD I2C | SCL | GPIO 9 | Pin I2C bawaan board DevKitC |

## Pembagi tegangan jalur ECHO

Pin ECHO HC-SR04 mengeluarkan sinyal 5V, sedangkan GPIO ESP32-S3 hanya toleran 3,3V.

```
ECHO ---[ R1 1k ]---+---[ R2 2k ]--- GND
                    |
                    +--- GPIO 6 (ESP32-S3)
```

Tegangan di GPIO = 5V x 2k / (1k + 2k) = sekitar 3,3V.

## Alamat I2C LCD

Alamat default kode: `0x27`. Jika layar tidak menampilkan apa pun (backlight menyala tapi kosong):

1. Coba ganti `LCD_ADDR` menjadi `0x3F`.
2. Putar potensiometer kontras di belakang modul I2C.
