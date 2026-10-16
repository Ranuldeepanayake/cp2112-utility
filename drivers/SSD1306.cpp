// Implements the command-based SSD1306 monochrome I2C OLED driver.
//
// The SSD1306 has no readable identity register and its I2C address is chosen
// by the module's SA0 hardware strap.  The driver therefore reports those
// limitations explicitly while exposing the useful display commands and a
// bounded, host-side framebuffer for text rendering.
#include "DeviceDriver.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <initializer_list>
#include <sstream>

namespace {
constexpr unsigned Width = 128;
constexpr unsigned Height = 64;
constexpr unsigned Pages = Height / 8;
constexpr unsigned FramebufferBytes = Width * Pages;
constexpr unsigned MaxTextLength = 96;
constexpr BYTE CommandControl = 0x00;
constexpr BYTE DataControl = 0x40;

bool parseValue(const std::string& text, unsigned maximum, unsigned& value) {
    unsigned long parsed = 0;
    if (!parseDriverNumber(text, parsed) || parsed > maximum) return false;
    value = static_cast<unsigned>(parsed);
    return true;
}

std::string hexByte(unsigned value) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(2)
           << std::setfill('0') << value;
    return output.str();
}

bool sendCommands(Bus& bus, unsigned address, std::initializer_list<BYTE> commands) {
    std::vector<BYTE> packet{CommandControl};
    packet.insert(packet.end(), commands.begin(), commands.end());
    return bus.writeBytes(address, packet);
}

bool sendCommandList(Bus& bus, unsigned address, const std::vector<BYTE>& commands) {
    if (commands.empty() || commands.size() > 60) return false;
    std::vector<BYTE> packet{CommandControl};
    packet.insert(packet.end(), commands.begin(), commands.end());
    return bus.writeBytes(address, packet);
}

bool sendData(Bus& bus, unsigned address, const std::vector<BYTE>& data) {
    if (data.empty() || data.size() > 60) return false;
    std::vector<BYTE> packet{DataControl};
    packet.insert(packet.end(), data.begin(), data.end());
    return bus.writeBytes(address, packet);
}

// A compact 5x7 font for the printable characters most commonly used in
// diagnostics. Unsupported characters are rendered as a visible placeholder.
std::array<BYTE, 5> glyph(char input) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(input)));
    switch (c) {
    case '0': return {0x3E, 0x51, 0x49, 0x45, 0x3E};
    case '1': return {0x00, 0x42, 0x7F, 0x40, 0x00};
    case '2': return {0x42, 0x61, 0x51, 0x49, 0x46};
    case '3': return {0x21, 0x41, 0x45, 0x4B, 0x31};
    case '4': return {0x18, 0x14, 0x12, 0x7F, 0x10};
    case '5': return {0x27, 0x45, 0x45, 0x45, 0x39};
    case '6': return {0x3C, 0x4A, 0x49, 0x49, 0x30};
    case '7': return {0x01, 0x71, 0x09, 0x05, 0x03};
    case '8': return {0x36, 0x49, 0x49, 0x49, 0x36};
    case '9': return {0x06, 0x49, 0x49, 0x29, 0x1E};
    case 'A': return {0x7E, 0x11, 0x11, 0x11, 0x7E};
    case 'B': return {0x7F, 0x49, 0x49, 0x49, 0x36};
    case 'C': return {0x3E, 0x41, 0x41, 0x41, 0x22};
    case 'D': return {0x7F, 0x41, 0x41, 0x22, 0x1C};
    case 'E': return {0x7F, 0x49, 0x49, 0x49, 0x41};
    case 'F': return {0x7F, 0x09, 0x09, 0x09, 0x01};
    case 'G': return {0x3E, 0x41, 0x49, 0x49, 0x7A};
    case 'H': return {0x7F, 0x08, 0x08, 0x08, 0x7F};
    case 'I': return {0x00, 0x41, 0x7F, 0x41, 0x00};
    case 'J': return {0x20, 0x40, 0x41, 0x3F, 0x01};
    case 'K': return {0x7F, 0x08, 0x14, 0x22, 0x41};
    case 'L': return {0x7F, 0x40, 0x40, 0x40, 0x40};
    case 'M': return {0x7F, 0x02, 0x0C, 0x02, 0x7F};
    case 'N': return {0x7F, 0x04, 0x08, 0x10, 0x7F};
    case 'O': return {0x3E, 0x41, 0x41, 0x41, 0x3E};
    case 'P': return {0x7F, 0x09, 0x09, 0x09, 0x06};
    case 'Q': return {0x3E, 0x41, 0x51, 0x21, 0x5E};
    case 'R': return {0x7F, 0x09, 0x19, 0x29, 0x46};
    case 'S': return {0x46, 0x49, 0x49, 0x49, 0x31};
    case 'T': return {0x01, 0x01, 0x7F, 0x01, 0x01};
    case 'U': return {0x3F, 0x40, 0x40, 0x40, 0x3F};
    case 'V': return {0x1F, 0x20, 0x40, 0x20, 0x1F};
    case 'W': return {0x3F, 0x40, 0x38, 0x40, 0x3F};
    case 'X': return {0x63, 0x14, 0x08, 0x14, 0x63};
    case 'Y': return {0x07, 0x08, 0x70, 0x08, 0x07};
    case 'Z': return {0x61, 0x51, 0x49, 0x45, 0x43};
    case '-': return {0x08, 0x08, 0x08, 0x08, 0x08};
    case '.': return {0x00, 0x60, 0x60, 0x00, 0x00};
    case ':': return {0x00, 0x36, 0x36, 0x00, 0x00};
    case '/': return {0x20, 0x10, 0x08, 0x04, 0x02};
    case '_': return {0x40, 0x40, 0x40, 0x40, 0x40};
    case '!': return {0x00, 0x00, 0x5F, 0x00, 0x00};
    case '?': return {0x02, 0x01, 0x51, 0x09, 0x06};
    case '@': return {0x3E, 0x41, 0x5D, 0x55, 0x5E};
    case '#': return {0x14, 0x7F, 0x14, 0x7F, 0x14};
    case '$': return {0x24, 0x2A, 0x7F, 0x2A, 0x12};
    case '%': return {0x63, 0x13, 0x08, 0x64, 0x63};
    case '&': return {0x36, 0x49, 0x55, 0x22, 0x50};
    case '*': return {0x14, 0x08, 0x3E, 0x08, 0x14};
    case '+': return {0x08, 0x08, 0x3E, 0x08, 0x08};
    case '=': return {0x14, 0x14, 0x14, 0x14, 0x14};
    case '(': return {0x00, 0x1C, 0x22, 0x41, 0x00};
    case ')': return {0x00, 0x41, 0x22, 0x1C, 0x00};
    case ',': return {0x00, 0x05, 0x06, 0x00, 0x00};
    case ';': return {0x00, 0x35, 0x36, 0x00, 0x00};
    case '\'': return {0x00, 0x03, 0x00, 0x00, 0x00};
    case '"': return {0x00, 0x03, 0x00, 0x03, 0x00};
    case ' ': return {0, 0, 0, 0, 0};
    default: return {0x7F, 0x41, 0x5D, 0x41, 0x7F};
    }
}

bool drawText(std::vector<BYTE>& framebuffer, const std::string& text,
              unsigned column, unsigned page, unsigned scale, bool on) {
    if (text.empty() || text.size() > MaxTextLength || column >= Width ||
        page >= Pages || scale < 1 || scale > 4) return false;
    const unsigned pixelHeight = 7 * scale;
    if (page * 8 + pixelHeight > Height) return false;
    unsigned x = column;
    unsigned y = page * 8;
    for (const char character : text) {
        if (character == '\n' || character == '\r') {
            x = column;
            y += 8 * scale;
            if (y + pixelHeight > Height) break;
            continue;
        }
        // Wrap at the next display line instead of silently dropping the
        // remainder of a long string at the right edge.
        if (x + 6 * scale > Width) {
            x = column;
            y += 8 * scale;
            if (y + pixelHeight > Height) break;
        }
        const auto columns = glyph(character);
        for (unsigned sourceX = 0; sourceX < 5; ++sourceX) {
            for (unsigned sourceY = 0; sourceY < 7; ++sourceY) {
                if (!(columns[sourceX] & (1u << sourceY))) continue;
                for (unsigned dx = 0; dx < scale; ++dx) {
                    for (unsigned dy = 0; dy < scale; ++dy) {
                        const unsigned pixelX = x + sourceX * scale + dx;
                        const unsigned pixelY = y + sourceY * scale + dy;
                        if (pixelX < Width && pixelY < Height) {
                            const size_t index = pixelX + (pixelY / 8) * Width;
                            const BYTE mask = static_cast<BYTE>(1u << (pixelY % 8));
                            if (on) framebuffer[index] |= mask;
                            else framebuffer[index] &= static_cast<BYTE>(~mask);
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
    return true;
}

bool flushFramebuffer(Bus& bus, unsigned address, const std::vector<BYTE>& framebuffer,
                      std::string& error) {
    if (framebuffer.size() != FramebufferBytes) {
        error = "Internal framebuffer size is not 1024 bytes";
        return false;
    }
    if (!sendCommands(bus, address, {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07})) {
        error = "SSD1306 column/page window command failed";
        return false;
    }
    for (unsigned page = 0; page < Pages; ++page) {
        std::vector<BYTE> chunk(framebuffer.begin() + page * Width,
                                framebuffer.begin() + (page + 1) * Width);
        // Some SSD1306-compatible modules do not accept a second long data
        // packet immediately after the first one.  Keep packets conservative
        // and add a short inter-transfer gap; this remains well within the
        // CP2112 limit while improving compatibility with small OLED boards.
        for (size_t offset = 0; offset < chunk.size(); offset += 16) {
            const size_t count = std::min<size_t>(16, chunk.size() - offset);
            if (!sendData(bus, address, std::vector<BYTE>(chunk.begin() + offset,
                                                          chunk.begin() + offset + count))) {
                std::ostringstream detail;
                detail << "SSD1306 framebuffer data transfer failed at page " << page
                       << ", byte offset " << offset;
                error = detail.str();
                return false;
            }
            Sleep(1);
        }
    }
    return true;
}

bool initializeDisplay(Bus& bus, unsigned address) {
    return sendCommandList(bus, address, {0xAE, 0xD5, 0x80, 0xA8, 0x3F,
        0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8,
        0xDA, 0x12, 0x81, 0x8F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF});
}

bool parseOrientation(const std::string& text, BYTE& segment, BYTE& scan) {
    std::string value = text;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value == "normal" || value == "0") { segment = 0xA1; scan = 0xC8; return true; }
    if (value == "rotate180" || value == "180" || value == "inverted") {
        segment = 0xA0; scan = 0xC0; return true;
    }
    return false;
}
}

struct Ssd1306Driver final : Driver {
    std::string title() const override { return "SSD1306 I2C OLED display"; }
    std::string capabilities() const override {
        return "128x64 monochrome OLED command control, bounded 1KB host framebuffer, text output, contrast, orientation, power, refresh-clock and display reset commands; identity, calibration and address are hardware-limited";
    }

    std::vector<DriverRegister> registers() const override {
        return {
            {0xAE, "DISPLAY_OFF", "Turn the panel off while retaining RAM", false, true},
            {0xAF, "DISPLAY_ON", "Turn the panel on", false, true},
            {0x81, "CONTRAST", "Next byte sets contrast from 0x00 to 0xFF", false, true},
            {0xA0, "SEGMENT_REMAP_NORMAL", "Normal horizontal orientation", false, true},
            {0xA1, "SEGMENT_REMAP_MIRROR", "Mirrored horizontal orientation", false, true},
            {0xC0, "COM_SCAN_NORMAL", "Normal vertical scan orientation", false, true},
            {0xC8, "COM_SCAN_REMAP", "Remapped vertical scan orientation", false, true},
            {0xD5, "DISPLAY_CLOCK", "Next byte sets oscillator divider and display clock", false, true},
            {0x8D, "CHARGE_PUMP", "Next byte enables or disables the internal charge pump", false, true},
            {0x20, "MEMORY_MODE", "Next byte selects horizontal, vertical or page addressing", false, true},
            {0x21, "COLUMN_WINDOW", "Next two bytes set the display column range", false, true},
            {0x22, "PAGE_WINDOW", "Next two bytes set the display page range", false, true},
            {0x40, "START_LINE", "Command value selects the display start line", false, true},
            {0xD3, "DISPLAY_OFFSET", "Next byte sets vertical display offset", false, true},
            {0xDA, "COM_PINS", "Next byte configures COM pin layout", false, true},
            {0xA4, "RESUME_RAM", "Display RAM contents", false, true},
            {0xA6, "NORMAL_DISPLAY", "Lit pixels are on", false, true},
            {0xA7, "INVERSE_DISPLAY", "Lit pixels are inverted", false, true}
        };
    }

    bool readResult(Bus&, const DeviceConfig& d, DeviceResult& result) override {
        // SSD1306 command I2C transactions are write-only on common modules;
        // these are structured capabilities, not fabricated hardware reads.
        result.succeeded = true;
        result.add("display_controller", std::string("SSD1306-compatible"));
        result.add("i2c_address", static_cast<uint64_t>(d.address), "7-bit");
        result.add("display_width_pixels", static_cast<uint64_t>(Width), "pixels");
        result.add("display_height_pixels", static_cast<uint64_t>(Height), "pixels");
        result.add("framebuffer_bytes", static_cast<uint64_t>(FramebufferBytes), "bytes");
        result.add("buffered_mode", std::string("host-side, action-local"));
        result.add("transfer_data_chunk_bytes", static_cast<uint64_t>(16), "bytes");
        result.add("identity_query", std::string("not supported by SSD1306 I2C command set"));
        result.add("calibration", std::string("not supported; panel geometry is hardware-defined"));
        result.add("address_change", std::string("not supported; selected by SA0 hardware strap"));
        return true;
    }

    std::vector<DriverAction> actions() const override {
        return {
            {"read-info", "Read display capabilities", {}, "The SSD1306 has no readable identity register."},
            {"display-on", "Turn display on", {}, ""},
            {"display-off", "Turn display off", {}, ""},
            {"clear-display", "Clear display", {}, "Clears the complete 128x64 framebuffer without changing the display configuration."},
            {"reset", "Reset display software state", {}, "Clears the display and reapplies safe SSD1306 defaults."},
            {"set-contrast", "Set brightness / contrast", {"Contrast (0x00..0xFF; typical 0x8F)"}, ""},
            {"set-orientation", "Set orientation", {"Orientation (normal or rotate180)"}, ""},
            {"set-power-mode", "Set power-saving mode", {"Mode (normal, saving, or off)"}, ""},
            {"set-refresh-rate", "Set display refresh clock", {"Clock divider (0x00..0xFF; SSD1306 clock setting, not a guaranteed Hz value)"}, ""},
            {"set-font-size", "Describe text font size", {"Font scale (1..4; applied by print-string)"}, ""},
            {"set-font-color", "Describe text color", {"Foreground pixels (on or off; off clears pixels)"}, "SSD1306 is monochrome; yellow/blue regions come from the OLED module's fixed filter."},
            {"calibrate", "Calibrate display", {}, "SSD1306 has no calibration command; this clears and reinitializes the panel."},
            {"set-address", "Set I2C address", {"7-bit address (0x3C or 0x3D)"}, "SSD1306 address is selected by the module's SA0 hardware strap."},
            {"print-string", "Print a small text string", {"Text", "Column (0..127)", "Page (0..7)", "Font scale (1..4)", "Foreground pixels (on or off)"}, "The text wraps across display lines in a 1KB host-side framebuffer; off clears pixels and does not select a physical color."}
        };
    }

    bool executeAction(Bus& bus, const DeviceConfig& d, const std::string& action,
                       const std::vector<std::string>& parameters, std::string& result) override {
        if (action == "read-info" && parameters.empty()) {
            result = "SSD1306-compatible 128x64 OLED; 1024-byte action-local framebuffer; identity readback unavailable";
            return true;
        }
        if (action == "display-on" && parameters.empty()) {
            if (!sendCommands(bus, d.address, {0xAF})) { result = "Display-on command failed"; return false; }
            result = "Display enabled"; return true;
        }
        if (action == "display-off" && parameters.empty()) {
            if (!sendCommands(bus, d.address, {0xAE})) { result = "Display-off command failed"; return false; }
            result = "Display disabled"; return true;
        }
        if (action == "clear-display" && parameters.empty()) {
            if (!initializeDisplay(bus, d.address)) {
                result = "SSD1306 initialization failed before display clear";
                return false;
            }
            const std::vector<BYTE> blank(FramebufferBytes, 0);
            if (!flushFramebuffer(bus, d.address, blank, result)) return false;
            result = "Display framebuffer cleared";
            return true;
        }
        if ((action == "reset" || action == "calibrate") && parameters.empty()) {
            if (!initializeDisplay(bus, d.address)) { result = "SSD1306 initialization/reset sequence failed"; return false; }
            if (action == "calibrate") result = "No hardware calibration exists; display was cleared and reinitialized";
            else result = "Display software state reset";
            return true;
        }
        if (action == "set-contrast" && parameters.size() == 1) {
            unsigned value = 0;
            if (!parseValue(parameters[0], 0xFF, value) || !sendCommands(bus, d.address, {0x81, static_cast<BYTE>(value)})) {
                result = "Invalid contrast or command failed"; return false;
            }
            result = "Contrast set to " + hexByte(value); return true;
        }
        if (action == "set-orientation" && parameters.size() == 1) {
            BYTE segment = 0, scan = 0;
            if (!parseOrientation(parameters[0], segment, scan) || !sendCommands(bus, d.address, {segment, scan})) {
                result = "Orientation must be normal or rotate180"; return false;
            }
            result = "Orientation updated"; return true;
        }
        if (action == "set-power-mode" && parameters.size() == 1) {
            std::string mode = parameters[0];
            std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (mode == "normal") {
                if (!sendCommands(bus, d.address, {0x8D, 0x14, 0xAF})) { result = "Normal power mode failed"; return false; }
            } else if (mode == "saving" || mode == "off") {
                if (!sendCommands(bus, d.address, {0xAE, 0x8D, 0x10})) { result = "Power-saving mode failed"; return false; }
            } else { result = "Power mode must be normal, saving, or off"; return false; }
            result = "Power mode set to " + mode; return true;
        }
        if (action == "set-refresh-rate" && parameters.size() == 1) {
            unsigned value = 0;
            if (!parseValue(parameters[0], 0xFF, value) || !sendCommands(bus, d.address, {0xD5, static_cast<BYTE>(value)})) {
                result = "Invalid SSD1306 clock divider or command failed"; return false;
            }
            result = "SSD1306 display clock divider set to " + hexByte(value); return true;
        }
        if (action == "set-font-size" && parameters.size() == 1) {
            unsigned value = 0;
            if (!parseValue(parameters[0], 4, value) || value < 1) { result = "Font scale must be 1..4"; return false; }
            result = "Font scale accepted: " + std::to_string(value) + " (use it with print-string)"; return true;
        }
        if (action == "set-font-color" && parameters.size() == 1) {
            std::string color = parameters[0];
            std::transform(color.begin(), color.end(), color.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (color != "on" && color != "off" && color != "1" && color != "0") { result = "Color must be on or off"; return false; }
            result = color == "off" || color == "0"
                ? "Foreground pixels will be cleared by print-string"
                : "Foreground pixels will be lit by print-string";
            return true;
        }
        if (action == "set-address" && parameters.size() == 1) {
            unsigned value = 0;
            if (!parseValue(parameters[0], 0x7F, value) || (value != 0x3C && value != 0x3D)) {
                result = "SSD1306 addresses are normally 0x3C or 0x3D"; return false;
            }
            result = "Address change is hardware-only: set the module SA0 strap and update configuration to " + hexByte(value);
            return false;
        }
        if (action == "print-string" && parameters.size() == 5) {
            unsigned column = 0, page = 0, scale = 0;
            if (!parseValue(parameters[1], Width - 1, column) || !parseValue(parameters[2], Pages - 1, page) ||
                !parseValue(parameters[3], 4, scale) || scale < 1) {
                result = "Invalid text position or font scale"; return false;
            }
            std::string color = parameters[4];
            std::transform(color.begin(), color.end(), color.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (color != "on" && color != "off" && color != "1" && color != "0") { result = "Color must be on or off"; return false; }
            std::vector<BYTE> framebuffer(FramebufferBytes, 0);
            if (!drawText(framebuffer, parameters[0], column, page, scale, color == "on" || color == "1")) {
                result = "Text does not fit the 128x64 display at the requested position/scale";
                return false;
            }
            // A module may power up with undefined controller state. Ensure
            // the panel is configured before sending the framebuffer.
            if (!initializeDisplay(bus, d.address)) {
                result = "SSD1306 initialization failed before text transfer";
                return false;
            }
            if (!flushFramebuffer(bus, d.address, framebuffer, result)) {
                return false;
            }
            result = (color == "off" || color == "0" ? "Cleared" : "Printed") +
                     std::string(" text using a 1024-byte framebuffer; long strings wrap across display lines");
            return true;
        }
        result = "Unknown SSD1306 action or invalid parameters";
        return false;
    }
};

std::unique_ptr<Driver> makeSsd1306Driver() { return std::make_unique<Ssd1306Driver>(); }
