# gopro-cli

Керування GoPro HERO7 Black з Linux через Wi-Fi. У проєкті дві програми:

- `gopro-gui` — вікно з живим зображенням, кнопками зйомки, усіма
  налаштуваннями камери та файлами з SD-карти;
- `gopro` — те саме з командного рядка.

Обидві працюють через спільну бібліотеку `libgopro` (`src/camera.*`).

> Статус: v0.5. Командний рядок перевірено на справжній HERO7 Black (прошивка
> HD7.01.01.90.00). Графічний інтерфейс перевірено лише на емуляторі камери.

## Збірка (Debian 13)

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev nlohmann-json3-dev \
    libsdl2-dev libgl-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev ffmpeg
cmake -S . -B build
cmake --build build -j
sudo cmake --install build      # ставить gopro і gopro-gui в /usr/local/bin
```

Лише командний рядок, без графіки: `cmake -S . -B build -DGOPRO_BUILD_GUI=OFF`.

Після перенесення теки проєкту видали `build` і повтори `cmake -S . -B build`.

## Графічний інтерфейс

```bash
gopro-gui                 # на екрані 4K: gopro-gui --scale 1.5
```

- Ліворуч живе зображення, під ним кнопка зйомки, режими та підрежими.
- Праворуч вкладка Settings (усі налаштування, з пошуком) і Media (файли на
  карті, Save зберігає в `~/GoPro`, Delete питає підтвердження).
- Унизу результат останньої дії або причина, чому камера недоступна.
- Якщо камера ще не в мережі, програма чекає і підключається сама.

## Підключення

1. Увімкни на камері бездротове з'єднання (меню Connections) і знайди там
   назву мережі камери та пароль.
2. Підключи ноутбук до Wi-Fi камери:
   `nmcli device wifi connect "НАЗВА_МЕРЕЖІ" password "ПАРОЛЬ"`
3. Перевір: `gopro info`

Поки ноутбук у мережі камери, інтернету через Wi-Fi не буде (через кабель
Ethernet інтернет лишається).

## Команди

```
gopro status                      батарея, режим, місце на карті
gopro record start | stop         запис відео
gopro photo                       зробити фото
gopro preview                     живе зображення з камери у вікні
gopro mode                        список режимів і підрежимів
gopro mode video timewarp         перемкнути режим
gopro settings video              налаштування відео і поточні значення
gopro get video.fps               варіанти одного налаштування
gopro set video.resolution 1080   змінити налаштування
gopro set video.eis on
gopro media ls                    файли на карті
gopro media get last -o ~/Videos  завантажити останній файл
gopro media get all  -o ~/Videos  завантажити все (вже завантажені пропускає)
gopro media rm GH010001.MP4       видалити файл з карти (питає підтвердження)
gopro time                        виставити годинник камери
gopro tag                         HiLight-мітка під час запису
gopro -v status                   показати всі запити до камери (діагностика)
gopro --help                      повний список
```

Назви налаштувань і варіантів утиліта не зберігає у коді. Камера сама віддає
їх опис за адресою `/gp/gpControl`, тому `gopro settings` показує саме те, що
вміє твоя прошивка. Опис кешується в `~/.cache/gopro-cli/`; після оновлення
прошивки запусти будь-яку команду з `--refresh`.

## Чекліст перевірки на камері

Виконай по черзі і надішли вивід того, що не спрацювало:

```bash
gopro info
gopro status
gopro status --all
gopro settings
gopro set video.resolution 1080
gopro set video.fps 60
gopro record start      # камера має почати запис
gopro status
gopro record stop
gopro photo
gopro media ls
gopro media get last
```

Якщо `gopro info` не відповідає, спробуй `gopro pair` (перед цим почни
підключення GoPro App у меню Connections камери).

Ще не перевірено на залізі: `gopro wake`.

## Тест без камери

```bash
python3 tools/mock_camera.py &
export GOPRO_HOST=127.0.0.1 GOPRO_PORT=8099 GOPRO_MEDIA_PORT=8099
./build/gopro status
```

## Структура

```
src/http.*      HTTP GET і завантаження файлів (libcurl)
src/camera.*    клас Camera: схема, стан, команди, медіа
src/main.cpp    командний рядок (gopro)
src/gui/        графічний інтерфейс (gopro-gui):
                main_gui.cpp      вікно та елементи керування (Dear ImGui)
                worker.*          фоновий потік для запитів до камери
                video_stream.*    прийом і декодування відео (FFmpeg)
third_party/    Dear ImGui (ліцензія MIT), включено у проєкт
tools/          емулятор камери і схема HERO7 Black для тестів
```

## Далі за планом

1. Перевірка графічного інтерфейсу на камері, затримка зображення.
2. Приховування варіантів, які камера не дозволяє в поточному режимі.
3. Bluetooth LE: будити камеру і вмикати її Wi-Fi без кнопок.

## Джерела

Протокол задокументований спільнотою:
[KonradIT/goprowifihack](https://github.com/KonradIT/goprowifihack) (Apache-2.0).
Файл `tools/hero7black-schema.json` взято звідти без змін.
