#include "overlay_enc.h"

#include "jpge.h"

#include <string.h>

namespace {

class buf_stream : public jpge::output_stream {
public:
    uint8_t *dst;
    size_t cap;
    size_t n;
    bool overflow;

    buf_stream() : dst(0), cap(0), n(0), overflow(false) {}

    virtual bool put_buf(const void *p, int len)
    {
        if (len < 0)
            return false;
        if (len == 0)
            return true;
        if ((size_t)len > cap - n) {
            overflow = true;
            return false;
        }
        memcpy(dst + n, p, (size_t)len);
        n += (size_t)len;
        return true;
    }

    virtual jpge::uint get_size() const { return (jpge::uint)n; }
};

buf_stream s_stream;
jpge::jpeg_encoder s_enc;
bool s_open;

} // namespace

extern "C" esp_err_t ov_jpg_begin(int w, int h, uint8_t *dst, size_t cap)
{
    jpge::params p;
    s_stream.dst = dst;
    s_stream.cap = cap;
    s_stream.n = 0;
    s_stream.overflow = false;
    p.m_quality = 100;
    p.m_subsampling = jpge::H2V2;
    s_open = false;
    if (!s_enc.init(&s_stream, w, h, 3, p)) {
        s_enc.deinit();
        return ESP_FAIL;
    }
    s_open = true;
    return ESP_OK;
}

extern "C" esp_err_t ov_jpg_line(const uint8_t *rgb)
{
    if (!s_open || !s_enc.process_scanline(rgb))
        return ESP_FAIL;
    return ESP_OK;
}

extern "C" esp_err_t ov_jpg_end(size_t *out_n)
{
    bool ok = s_open && s_enc.process_scanline(0);
    s_enc.deinit();
    s_open = false;
    if (!ok || s_stream.overflow || s_stream.n < 4)
        return ESP_FAIL;
    if (out_n)
        *out_n = s_stream.n;
    return ESP_OK;
}

extern "C" void ov_jpg_abort(void)
{
    if (!s_open)
        return;
    s_enc.deinit();
    s_open = false;
}
