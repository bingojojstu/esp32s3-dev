#ifndef CAMERA_H
#define CAMERA_H

#include <string>
#include <vector>
#include <cstdint>

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::string Explain(const std::string& question) = 0;

    // Capture a frame and JPEG-encode it into the caller's buffer.
    // Synchronous (~200-400ms depending on resolution). Returns false if
    // the board's camera doesn't support synchronous JPEG export. Used by
    // the OpenClaw VLM path; the Xiaozhi-style Explain() above still
    // streams over HTTP independently and isn't affected.
    virtual bool CaptureToJpeg(std::vector<uint8_t>& /*jpeg_bytes*/) { return false; }
};

#endif // CAMERA_H
