#include "blocked_gzip_output_stream.hpp"

#include "hfile_cppstream.hpp"

// We need the hFILE* internals available.
#include <hfile_internal.h>

namespace vg {

namespace stream {

using namespace std;

BlockedGzipOutputStream::BlockedGzipOutputStream(BGZF* bgzf_handle) : handle(bgzf_handle), buffer(), backed_up(0), byte_count(0),
    know_offset(false) {
    
    if (handle->mt) {
        // I don't want to deal with BGZF multithreading, because I'm going to be hacking its internals
        throw runtime_error("Multithreaded BGZF is not supported");
    }
    
    // Force the BGZF to start a new block by flushing the old one, if it exists.
    if (bgzf_flush(handle) != 0) {
        throw runtime_error("Unable to flush BGZF");
    }
   
    // Try seeking the hfile's backend to exactly the position it is at, to get the actual offset.
    // This lets us know if the stream is really seekable/tellable, because htell always works.
    auto cur_pos = (*(handle->fp->backend->seek))(handle->fp, 0, SEEK_CUR);
    if (cur_pos >= 0) {
        // The seek succeeded. We know where we are, and so, we assume, does
        // the hFILE.
        
        // Tell the BGZF where it is (which is at the hFILE's position rather
        // than the backend's, but we know the hFILE position is correct)
        handle->block_address = htell(handle->fp);
        
        // We are backed by a tellable stream
        know_offset = true;
    }
}

BlockedGzipOutputStream::BlockedGzipOutputStream(std::ostream& stream) : handle(nullptr), buffer(), backed_up(0), byte_count(0),
    know_offset(false) {
    
    // Wrap the stream in an hFILE*
    hFILE* wrapped = hfile_wrap(stream);
    if (wrapped == nullptr) {
        throw runtime_error("Unable to wrap stream");
    }
    
    // Give ownership of it to a BGZF that writes, which we in turn own.
    handle = bgzf_hopen(wrapped, "w");
    if (handle == nullptr) {
        throw runtime_error("Unable to set up BGZF library on wrapped stream");
    }
    
    stream.clear();
    auto file_start = stream.tellp();
    if (file_start >= 0 && stream.good()) {
        // The stream we are wrapping is seekable.
        
        // We need to make sure BGZF knows where its blocks are starting.
    
        // No need to flush because we just freshly opened the BGZF
        
        // Tell the BGZF where its next block is actually starting.
        handle->block_address = file_start;
        
        // Remember the virtual offsets will be valid
        know_offset = true;
    }
}

BlockedGzipOutputStream::~BlockedGzipOutputStream() {
    // Make sure to finish writing before destructing.
    flush();
    // Close the GBZF
    bgzf_close(handle);
}

bool BlockedGzipOutputStream::Next(void** data, int* size) {
    try {
        // Dump data if we have it
        flush();
        
        // Allocate some space in the buffer
        buffer.resize(4096);
        
        // None of it is backed up
        backed_up = 0;
        
        // Tell the caller where to write
        *data = (void*)&buffer[0];
        *size = buffer.size();
        
        // It worked
        return true;
        
    } catch(exception e) {
        return false;
    }
}

void BlockedGzipOutputStream::BackUp(int count) {
    backed_up += count;
    assert(backed_up <= buffer.size());
}

int64_t BlockedGzipOutputStream::ByteCount() const {
    return byte_count;
}

bool BlockedGzipOutputStream::WriteAliasedRaw(const void* data, int size) {
    // Not allowed
    return false;
}

bool BlockedGzipOutputStream::AllowsAliasing() const {
    return false;
}


int64_t BlockedGzipOutputStream::Tell() {
    if (know_offset) {
        // Our virtual offsets are true.
        
        // Make sure all data has been sent to BGZF
        flush();
        
        // See where we are now
        return bgzf_tell(handle);
    } else {
        // We don't know where the zero position in the stream was, so we can't
        // trust BGZF's virtual offsets.
        return -1;
    }
}

void BlockedGzipOutputStream::flush() {
    // How many bytes are left to write?
    auto outstanding = buffer.size() - backed_up;
    if (outstanding > 0) {
        // Save the buffer
        auto written = bgzf_write(handle, (void*)&buffer[0], outstanding);
        
        if (written != outstanding) {
            // This only happens when there is an error
            throw runtime_error("IO error writing data in BlockedGzipOutputStream");
        }
        
        // Record the actual write
        byte_count += written;
    }
}

}

}
