/*
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * MAX7456 driver partially based on betaflight and inav max7456.c implemention.
 * Many thanks to their authors.
 */

#include <AP_OSD/AP_OSD_MAX7456.h>

#include <AP_HAL/Util.h>
#include <AP_HAL/Semaphores.h>
#include <AP_HAL/Scheduler.h>
#include <AP_ROMFS/AP_ROMFS.h>

#include <utility>

#define VIDEO_BUFFER_CHARS_NTSC   390
#define VIDEO_BUFFER_CHARS_PAL    480
#define VIDEO_LINES_NTSC          13
#define VIDEO_LINES_PAL           16
#define VIDEO_COLUMNS             30
#define MAX_UPDATED_CHARS         64
#define SPI_BUFFER_SIZE           ((MAX_UPDATED_CHARS + 1)* 8)
#define NVM_RAM_SIZE              54

//MAX7456 registers
#define MAX7456ADD_READ         0x80
#define MAX7456ADD_VM0          0x00
#define MAX7456ADD_VM1          0x01
#define MAX7456ADD_HOS          0x02
#define MAX7456ADD_VOS          0x03
#define MAX7456ADD_DMM          0x04
#define MAX7456ADD_DMAH         0x05
#define MAX7456ADD_DMAL         0x06
#define MAX7456ADD_DMDI         0x07
#define MAX7456ADD_CMM          0x08
#define MAX7456ADD_CMAH         0x09
#define MAX7456ADD_CMAL         0x0a
#define MAX7456ADD_CMDI         0x0b
#define MAX7456ADD_OSDM         0x0c
#define MAX7456ADD_RB0          0x10
#define MAX7456ADD_OSDBL        0x6c
#define MAX7456ADD_STAT         0xA0

// VM0 register bits
#define VIDEO_BUFFER_DISABLE        0x01
#define MAX7456_RESET               0x02
#define VERTICAL_SYNC_NEXT_VSYNC    0x04
#define OSD_ENABLE                  0x08
#define VIDEO_MODE_PAL              0x40
#define VIDEO_MODE_NTSC             0x00
#define VIDEO_MODE_MASK             0x40

#define VIDEO_MODE_IS_PAL(val)      (((val) & VIDEO_MODE_MASK) == VIDEO_MODE_PAL)
#define VIDEO_MODE_IS_NTSC(val) (((val) & VIDEO_MODE_MASK) == VIDEO_MODE_NTSC)

// VM1 register bits
// duty cycle is on_off
#define BLINK_DUTY_CYCLE_50_50 0x00
#define BLINK_DUTY_CYCLE_33_66 0x01
#define BLINK_DUTY_CYCLE_25_75 0x02
#define BLINK_DUTY_CYCLE_75_25 0x03

// blinking time
#define BLINK_TIME_0 0x00
#define BLINK_TIME_1 0x04
#define BLINK_TIME_2 0x08
#define BLINK_TIME_3 0x0C

// background mode brightness (percent)
#define BACKGROUND_BRIGHTNESS_0  (0x00 << 4)
#define BACKGROUND_BRIGHTNESS_7  (0x01 << 4)
#define BACKGROUND_BRIGHTNESS_14 (0x02 << 4)
#define BACKGROUND_BRIGHTNESS_21 (0x03 << 4)
#define BACKGROUND_BRIGHTNESS_28 (0x04 << 4)
#define BACKGROUND_BRIGHTNESS_35 (0x05 << 4)
#define BACKGROUND_BRIGHTNESS_42 (0x06 << 4)
#define BACKGROUND_BRIGHTNESS_49 (0x07 << 4)

// STAT register bits
#define STAT_PAL      0x01
#define STAT_NTSC     0x02
#define STAT_LOS      0x04
#define STAT_NVR_BUSY 0x20

#define STAT_IS_PAL(val)  ((val) & STAT_PAL)
#define STAT_IS_NTSC(val) ((val) & STAT_NTSC)
#define STAT_IS_LOS(val)  ((val) & STAT_LOS)

#define VIN_IS_PAL(val)  (!STAT_IS_LOS(val) && STAT_IS_PAL(val))
#define VIN_IS_NTSC(val)  (!STAT_IS_LOS(val) && STAT_IS_NTSC(val))

// There are occasions that NTSC is not detected even with !LOS (AB7456 specific?)
// When this happens, lower 3 bits of STAT register is read as zero.
// To cope with this case, this macro defines !LOS && !PAL as NTSC.
// Should be compatible with MAX7456 and non-problematic case.
#define VIN_IS_NTSC_ALT(val)  (!STAT_IS_LOS(val) && !STAT_IS_PAL(val))

//CMM register bits
#define WRITE_NVR               0xA0

// DMM special bits
#define DMM_BLINK (1 << 4)
#define DMM_INVERT_PIXEL_COLOR (1 << 3)
#define DMM_CLEAR_DISPLAY (1 << 2)
#define DMM_CLEAR_DISPLAY_VERT (DMM_CLEAR_DISPLAY | 1 << 1)
#define DMM_AUTOINCREMENT (1 << 0)

// time to check video signal format
#define VIDEO_SIGNAL_CHECK_INTERVAL_MS 1000
//time to wait for input to stabilize
#define VIDEO_SIGNAL_DEBOUNCE_MS 100
//time to wait nvm flash complete
#define MAX_NVM_WAIT 10000

//black and white level
#ifndef WHITEBRIGHTNESS
#define WHITEBRIGHTNESS 0x01
#endif
#ifndef BLACKBRIGHTNESS
#define BLACKBRIGHTNESS 0x00
#endif
#define BWBRIGHTNESS ((BLACKBRIGHTNESS << 2) | WHITEBRIGHTNESS)


extern const AP_HAL::HAL &hal;

AP_OSD_MAX7456::AP_OSD_MAX7456(AP_OSD &osd, AP_HAL::OwnPtr<AP_HAL::Device> dev):
    AP_OSD_Backend(osd), _dev(std::move(dev))
{
    buffer = nullptr;
    frame = nullptr;
    shadow_frame = nullptr;
    attr = nullptr;;
    shadow_attr = nullptr;
    video_signal_reg = VIDEO_MODE_PAL | OSD_ENABLE;
    max_screen_size = VIDEO_BUFFER_CHARS_PAL;
    buffer_offset = 0;
    video_detect_time = 0;
    last_signal_check = 0;
    initialized = false;
}

AP_OSD_MAX7456::~AP_OSD_MAX7456()
{
    if (buffer != nullptr) {
        hal.util->free_type(buffer, SPI_BUFFER_SIZE, AP_HAL::Util::MEM_DMA_SAFE);
    }
    if (frame != nullptr) {
        hal.util->free_type(frame, VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);
    }
    if (shadow_frame != nullptr) {
        hal.util->free_type(shadow_frame, VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);
    }
    if (attr != nullptr) {
        hal.util->free_type(attr, VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);
    }
    if (shadow_attr != nullptr) {
        hal.util->free_type(shadow_attr, VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);
    }

}

bool AP_OSD_MAX7456::init()
{
    uint8_t status = 0xFF;
    buffer = (uint8_t *)hal.util->malloc_type(SPI_BUFFER_SIZE, AP_HAL::Util::MEM_DMA_SAFE);
    frame = (uint8_t *)hal.util->malloc_type(VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);
    shadow_frame = (uint8_t *)hal.util->malloc_type(VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);
    attr = (uint8_t *)hal.util->malloc_type(VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);
    shadow_attr = (uint8_t *)hal.util->malloc_type(VIDEO_BUFFER_CHARS_PAL, AP_HAL::Util::MEM_FAST);


    if (buffer == nullptr || frame == nullptr || shadow_frame == nullptr || attr== nullptr ||shadow_attr == nullptr) {
        return false;
    }

    _dev->set_speed(AP_HAL::Device::SPEED_HIGH);

    _dev->get_semaphore()->take_blocking();
    _dev->write_register(MAX7456ADD_VM0, MAX7456_RESET);
    hal.scheduler->delay(1);
    _dev->read_registers(MAX7456ADD_VM0|MAX7456ADD_READ, &status, 1);
    _dev->get_semaphore()->give();
    return status == 0;
}

bool AP_OSD_MAX7456::update_font()
{
    uint32_t font_size;
    const uint8_t *font_data = AP_ROMFS::find_file("osd_font.bin", font_size);
    if (font_data == nullptr || font_size != NVM_RAM_SIZE * 256) {
        return false;
    }

    for (int chr=0; chr < 256; chr++) {
        uint8_t status;
        const uint8_t* chr_font_data = font_data + chr*NVM_RAM_SIZE;
        int retry;
        buffer_offset = 0;
        buffer_add_cmd(MAX7456ADD_VM0, 0);
        buffer_add_cmd(MAX7456ADD_CMAH, chr);
        for (int x = 0; x < NVM_RAM_SIZE; x++) {
            buffer_add_cmd(MAX7456ADD_CMAL, x);
            buffer_add_cmd(MAX7456ADD_CMDI, chr_font_data[x]);
        }
        buffer_add_cmd(MAX7456ADD_CMM, WRITE_NVR);

        _dev->get_semaphore()->take_blocking();
        _dev->transfer(buffer, buffer_offset, nullptr, 0);
        _dev->get_semaphore()->give();

        for (retry = 0; retry < MAX_NVM_WAIT; retry++) {
            hal.scheduler->delay(15);
            _dev->get_semaphore()->take_blocking();
            _dev->read_registers(MAX7456ADD_STAT, &status, 1);
            _dev->get_semaphore()->give();
            if ((status & STAT_NVR_BUSY) == 0x00) {
                break;
            }
        }
        if (retry == MAX_NVM_WAIT) {
            return false;
        }
    }

    return true;
}

AP_OSD_Backend *AP_OSD_MAX7456::probe(AP_OSD &osd, AP_HAL::OwnPtr<AP_HAL::Device> dev)
{
    if (!dev) {
        return nullptr;
    }

    AP_OSD_MAX7456 *backend = new AP_OSD_MAX7456(osd, std::move(dev));
    if (!backend || !backend->init()) {
        delete backend;
        return nullptr;
    }

    return backend;
}

void AP_OSD_MAX7456::buffer_add_cmd(uint8_t reg, uint8_t arg)
{
    if (buffer_offset < SPI_BUFFER_SIZE - 1) {
        buffer[buffer_offset++] = reg;
        buffer[buffer_offset++] = arg;
    }
}

void AP_OSD_MAX7456::check_reinit()
{
    uint8_t check = 0xFF;
    _dev->get_semaphore()->take_blocking();

    _dev->read_registers(MAX7456ADD_VM0|MAX7456ADD_READ, &check, 1);

    uint32_t now = AP_HAL::millis();

    // Stall check
    if (check != video_signal_reg) {
        reinit();
    } else if ((now - last_signal_check) > VIDEO_SIGNAL_CHECK_INTERVAL_MS) {
        uint8_t sense;

        // Adjust output format based on the current input format
        _dev->read_registers(MAX7456ADD_STAT, &sense, 1);

        if (sense & STAT_LOS) {
            video_detect_time = 0;
        } else {
            if ((VIN_IS_PAL(sense) && VIDEO_MODE_IS_NTSC(video_signal_reg))
                || (VIN_IS_NTSC_ALT(sense) && VIDEO_MODE_IS_PAL(video_signal_reg))) {
                if (video_detect_time) {
                    if (AP_HAL::millis() - video_detect_time > VIDEO_SIGNAL_DEBOUNCE_MS) {
                        reinit();
                    }
                } else {
                    // Wait for signal to stabilize
                    video_detect_time = AP_HAL::millis();
                }
            }
        }
        last_signal_check = now;
    }
    _dev->get_semaphore()->give();
}

void AP_OSD_MAX7456::reinit()
{
    uint8_t sense;

    //do not init MAX before camera power up correctly
    if (AP_HAL::millis() < 1500) {
        return;
    }

    //check input signal format
    _dev->read_registers(MAX7456ADD_STAT, &sense, 1);
    if (VIN_IS_PAL(sense)) {
        video_signal_reg = VIDEO_MODE_PAL | OSD_ENABLE;
        max_screen_size = VIDEO_BUFFER_CHARS_PAL;
    } else {
        video_signal_reg = VIDEO_MODE_NTSC | OSD_ENABLE;
        max_screen_size = VIDEO_BUFFER_CHARS_NTSC;
    }

    // set all rows to same character black/white level
    for (int x = 0; x < VIDEO_LINES_PAL; x++) {
        _dev->write_register(MAX7456ADD_RB0 + x, BWBRIGHTNESS);
    }

    // make sure the Max7456 is enabled
    _dev->write_register(MAX7456ADD_VM0, video_signal_reg);
    _dev->write_register(MAX7456ADD_VM1, BLINK_DUTY_CYCLE_50_50 | BLINK_TIME_3 | BACKGROUND_BRIGHTNESS_28);
    _dev->write_register(MAX7456ADD_DMM, DMM_CLEAR_DISPLAY);

    // force redrawing all screen
    memset(shadow_frame, 0xFF, VIDEO_BUFFER_CHARS_PAL);
    memset(shadow_attr, 0xFF, VIDEO_BUFFER_CHARS_PAL);

    initialized = true;
}

void AP_OSD_MAX7456::flush()
{
    if (_osd.update_font) {
        if (!update_font()) {
            hal.console->printf("AP_OSD: error during font update\n");
        }
        _osd.update_font.set_and_save(0);
    }
    check_reinit();
    transfer_frame();
}

void AP_OSD_MAX7456::transfer_frame()
{
    int updated_chars = 0;
    uint8_t last_attribute = 0xFF;
    if (!initialized) {
        return;
    }

    buffer_offset = 0;
    for (int pos=0; pos<max_screen_size; pos++) {
        if (frame[pos] == shadow_frame[pos] && attr[pos] == shadow_attr[pos]) {
            continue;
        }
        if (++updated_chars > MAX_UPDATED_CHARS) {
            break;
        }
        shadow_frame[pos] = frame[pos];
        shadow_attr[pos] = attr[pos];
        uint8_t attribute = attr[pos] & (DMM_BLINK | DMM_INVERT_PIXEL_COLOR);
        uint8_t chr = frame[pos];

        if (attribute != last_attribute) {
            buffer_add_cmd(MAX7456ADD_DMM, attribute);
            last_attribute = attribute;
        }
        buffer_add_cmd(MAX7456ADD_DMAH, pos >> 8);
        buffer_add_cmd(MAX7456ADD_DMAL, pos & 0xFF);
        buffer_add_cmd(MAX7456ADD_DMDI, chr);
    }

    if (buffer_offset > 0) {
        _dev->get_semaphore()->take_blocking();
        _dev->transfer(buffer, buffer_offset, nullptr, 0);
        _dev->get_semaphore()->give();
    }
}

void AP_OSD_MAX7456::clear()
{
    for (int i=0; i<VIDEO_BUFFER_CHARS_PAL; i++) {
        frame[i] = ' ';
        attr[i] = 0;
    }
}

void AP_OSD_MAX7456::write(int x, int y, const char* text, uint8_t char_attr)
{
    if (y >= VIDEO_LINES_PAL || text == nullptr) {
        return;
    }
    while ((x < VIDEO_COLUMNS) && (*text != 0)) {
        frame[y * VIDEO_COLUMNS + x] = *text;
        attr[y * VIDEO_COLUMNS + x] = char_attr;
        ++text;
        ++x;
    }
}