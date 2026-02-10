# YD-ESP32-23 (ESP32-S3-WROOM-1-N16R8) - PlatformIO

Projeto base para ESP32-S3 usando Arduino framework no PlatformIO.

## Requisitos
- VS Code
- Extensão PlatformIO IDE (recomendado para interface gráfica)
- `pio` no terminal (já instalado)

## Configuração atual da placa
- Módulo: `ESP32-S3-WROOM-1-N16R8`
- Flash: `16MB`
- PSRAM: `8MB` (OPI)
- Porta configurada: `COM16`

## Comandos úteis (WSL/Linux)
No diretório do projeto:

```bash
cd /mnt/d/Esp_Programs/yd-esp32-23
export PATH="$HOME/.local/bin:$PATH"

# Compilar
pio run

# Upload (se necessário, especifique a porta)
pio run -t upload
# pio run -t upload --upload-port COM7

# Monitor serial
pio device monitor
# pio device monitor --port COM7
```

## Upload no Windows (recomendado para COMx)
```bat
C:\Users\igorh\AppData\Local\Programs\Python\Python313\python.exe -m platformio run -d D:\Esp_Programs\yd-esp32-23 -t upload --upload-port COM16
```

## Observações
- Board base usada: `esp32-s3-devkitc-1`, com overrides para N16R8 no `platformio.ini`.
- Se aparecer erro `Failed to connect to ESP32-S3: No serial data received`, faça boot manual:
  1. Segure `BOOT`
  2. Pressione e solte `EN/RST`
  3. Solte `BOOT`
  4. Execute upload novamente

## Conectar no mesmo Wi-Fi
1. Crie `include/wifi_secrets.h` com base em `include/wifi_secrets.example.h`.
2. Preencha `WIFI_SSID` e `WIFI_PASSWORD` com os dados da sua rede.
3. Faça upload normal.
4. No monitor serial, procure:
   - `[OK] Wi-Fi conectado.`
   - `IP: ...`

## Interface web do LED
- Abra no navegador: `http://esp32.local`
- Fallback por IP: `http://192.168.20.101` (ou o IP mostrado no serial)
- API de estado: `GET /api/state`
- API para mudar cor: `GET /api/led?hex=RRGGBB`
  - Exemplo: `http://esp32.local/api/led?hex=FF0000`
