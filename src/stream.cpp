// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#include "stream.h"
#include "sync.h"       // For frame_archive
#include "image.h"      // For image alignment, rectification, and deprojection routines
#include <algorithm>    // For sort
#include <tuple>        // For make_tuple

using namespace rsimpl;

rs_extrinsics stream_interface::get_extrinsics_to(const rs_stream_interface & other) const
{
    if (!validator.validate_extrinsics(stream, other.get_stream_type()))
    {
        throw std::runtime_error(to_string() << "The extrinsic from " << get_stream_type() << " to " << other.get_stream_type() << " is not valid");
    }
    
    auto& r = dynamic_cast<const stream_interface&>(other);
    auto from = get_pose(), to = r.get_pose();
    if(from == to) return {{1,0,0,0,1,0,0,0,1},{0,0,0}};
    auto transform = inverse(from) * to;
    rs_extrinsics extrin;
    (float3x3 &)extrin.rotation = transform.orientation;
    (float3 &)extrin.translation = transform.position;
    return extrin;
}

native_stream::native_stream(device_config & config, rs_stream stream, calibration_validator in_validator) : stream_interface(in_validator, stream), config(config)
{
    for(auto & subdevice_mode : config.info.subdevice_modes)
    {
        for(auto pad_crop : subdevice_mode.pad_crop_options)
        {
            for(auto & unpacker : subdevice_mode.pf.unpackers)
            {
                auto selection = subdevice_mode_selection(subdevice_mode, pad_crop, (int)(&unpacker - subdevice_mode.pf.unpackers.data()));
                if(selection.provides_stream(stream)) modes.push_back(selection);
            }
        }
    }

    auto get_tuple = [stream](const subdevice_mode_selection & selection)
    {     
        return std::make_tuple(-selection.get_width(), -selection.get_height(), -selection.get_framerate(), selection.get_format(stream));
    };

    std::sort(begin(modes), end(modes), [get_tuple](const subdevice_mode_selection & a, const subdevice_mode_selection & b) { return get_tuple(a) < get_tuple(b); });
    auto it = std::unique(begin(modes), end(modes), [get_tuple](const subdevice_mode_selection & a, const subdevice_mode_selection & b) { return get_tuple(a) == get_tuple(b); });
    if(it != end(modes)) modes.erase(it, end(modes));
}

void native_stream::get_mode(int mode, int * w, int * h, rs_format * f, int * fps) const
{
    auto & selection = modes[mode];
    if(w) *w = selection.get_width();
    if(h) *h = selection.get_height();
    if(f) *f = selection.get_format(stream);
    if(fps) *fps = selection.get_framerate();
}

bool native_stream::is_enabled() const
{ 
    return (archive && archive->is_stream_enabled(stream)) || config.requests[stream].enabled; 
}

subdevice_mode_selection native_stream::get_mode() const
{
    if(archive && archive->is_stream_enabled(stream)) return archive->get_mode(stream);
    if(config.requests[stream].enabled)
    {
        for(auto subdevice_mode : config.select_modes())
        {
            if(subdevice_mode.provides_stream(stream)) return subdevice_mode;
        }   
        throw std::logic_error("no mode found"); // Should never happen, select_modes should throw if no mode can be found
    }
    throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
}

rs_intrinsics native_stream::get_intrinsics() const 
{
    if (!validator.validate_intrinsics(stream))
    {
        LOG_ERROR("The intrinsic of " << get_stream_type() << " is not valid");
    }
    const auto m = get_mode();
    return pad_crop_intrinsics(m.mode.native_intrinsics, m.pad_crop);
}

rs_intrinsics native_stream::get_rectified_intrinsics() const
{
    if (!validator.validate_intrinsics(stream))
    {
        throw std::runtime_error(to_string() << "The intrinsic of " << get_stream_type() << " is not valid");
    }
    const auto m = get_mode();
    if(m.mode.rect_modes.empty()) return get_intrinsics();
    return pad_crop_intrinsics(m.mode.rect_modes[0], m.pad_crop);
}

double native_stream::get_frame_metadata(rs_frame_metadata frame_metadata) const
{
    if (!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return archive->get_frame_metadata(stream, frame_metadata);
}

bool native_stream::supports_frame_metadata(rs_frame_metadata frame_metadata) const
{
    if (!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return archive->supports_frame_metadata(stream, frame_metadata);
}

unsigned long long native_stream::get_frame_number() const
{ 
    if (!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return archive->get_frame_number(stream);
}

double native_stream::get_frame_timestamp() const
{
    if (!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return archive->get_frame_timestamp(stream);
}

long long native_stream::get_frame_system_time() const
{
    if (!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return archive->get_frame_system_time(stream);
}

const uint8_t * native_stream::get_frame_data() const
{
    if(!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return (const uint8_t *) archive->get_frame_data(stream);
}

int native_stream::get_frame_stride() const
{
    if (!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return archive->get_frame_stride(stream);
}

int native_stream::get_frame_bpp() const
{
    if (!is_enabled()) throw std::runtime_error(to_string() << "stream not enabled: " << rs_stream_to_string(stream));
    if (!archive) throw  std::runtime_error(to_string() << "streaming not started!");
    return archive->get_frame_bpp(stream);
}

const uint8_t * point_stream::get_frame_data() const
{
    if(image.empty() || number != get_frame_number())
    {
        image.resize(get_image_size(get_intrinsics().width, get_intrinsics().height, get_format()));

        if(source.get_format() == RS_FORMAT_Z16)
        {
            deproject_z(reinterpret_cast<float *>(image.data()), get_intrinsics(), reinterpret_cast<const uint16_t *>(source.get_frame_data()), get_depth_scale());
        }
        else if(source.get_format() == RS_FORMAT_DISPARITY16)
        {
            deproject_disparity(reinterpret_cast<float *>(image.data()), get_intrinsics(), reinterpret_cast<const uint16_t *>(source.get_frame_data()), get_depth_scale());
        }
        else assert(false && "Cannot deproject image from a non-depth format");

        number = get_frame_number();
    }
    return image.data();
}

const uint8_t * rectified_stream::get_frame_data() const
{
    // If source image is already rectified, just return it without doing any work
    if(get_pose() == source.get_pose() && get_intrinsics() == source.get_intrinsics()) return source.get_frame_data();

    if(image.empty() || number != get_frame_number())
    {
        if(table.empty()) table = compute_rectification_table(get_intrinsics(), get_extrinsics_to(source), source.get_intrinsics());
        image.resize(get_image_size(get_intrinsics().width, get_intrinsics().height, get_format()));
        rectify_image(image.data(), table, source.get_frame_data(), get_format());
        number = get_frame_number();
    }
    return image.data();
}

const uint8_t * aligned_stream::get_frame_data() const
{
    if(image.empty() || number != get_frame_number())
    {
        image.resize(get_image_size(get_intrinsics().width, get_intrinsics().height, get_format()));
        memset(image.data(), from.get_format() == RS_FORMAT_DISPARITY16 ? 0xFF : 0x00, image.size());
        if(from.get_format() == RS_FORMAT_Z16)
        {
            align_z_to_other(image.data(), (const uint16_t *)from.get_frame_data(), from.get_depth_scale(), from.get_intrinsics(), from.get_extrinsics_to(to), to.get_intrinsics());
        }
        else if(from.get_format() == RS_FORMAT_DISPARITY16)
        {
            align_disparity_to_other(image.data(), (const uint16_t *)from.get_frame_data(), from.get_depth_scale(), from.get_intrinsics(), from.get_extrinsics_to(to), to.get_intrinsics());
        }
        else if(to.get_format() == RS_FORMAT_Z16)
        {
            align_other_to_z(image.data(), (const uint16_t *)to.get_frame_data(), to.get_depth_scale(), to.get_intrinsics(), to.get_extrinsics_to(from), from.get_intrinsics(), from.get_frame_data(), from.get_format());
        }
        else if(to.get_format() == RS_FORMAT_DISPARITY16)
        {
            align_other_to_disparity(image.data(), (const uint16_t *)to.get_frame_data(), to.get_depth_scale(), to.get_intrinsics(), to.get_extrinsics_to(from), from.get_intrinsics(), from.get_frame_data(), from.get_format());
        }
        else assert(false && "Cannot align two images if neither have depth data");
        number = get_frame_number();
    }
    return image.data();
}
