/*
 * Modern effects for a modern Streamer
 * Copyright (C) 2020 Michael Fabian Dirks
 * Copyright (C) 2023 Nate <natevoci @ github>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "filter-autoframing.hpp"
#include "obs/gs/gs-helper.hpp"
#include "util/util-logging.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<filter::autoframing> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

// Auto-Framing is the process of tracking important information inside of a group of video or
// audio samples, and then automatically cutting away all the unnecessary parts. In our case, we
// will focus on video only as the audio field is already covered by other solutions, like Noise
// Gate, Denoising, etc. The implementation will rely on the Provider system, so varying
// functionality should be expected from all providers. Some providers may only offer a way to
// track a single face, others will allow groups, yet others will allow even non-humans to be
// tracked.
//
// The goal is to provide Auto-Framing for single person streams ('Solo') as well as group streams
// ('Group'), though the latter will only be available if the provider supports it. In 'Solo' mode
// the filter will perfectly frame a single person, and no more than that. In 'Group' mode, it will
// combine all important elements into a single frame, and track that instead. In the future, we
// might want to offer a third mode to give each tracked face a separate frame however this may
// exceed the intended complexity of this feature entirely.

/** Settings
 * Framing
 *   Mode: How should things be tracked?
 *     Solo: Frame only a single face.
 *     Group: Frame many faces, group all into single frame.
 *   Padding: How many pixels/much % of tracked are should be kept
 *   Aspect Ratio: What Aspect Ratio should the framed output have?
 *   Stability: How stable is the framing against changes of tracked elements?
 * 
 * Motion
 *   Motion Prediction: How much should we attempt to predict where tracked elements move?
 *   Smoothing: How much should the position between tracking attempts 
 * 
 * Advanced
 *   Provider: What provider should be used?
 *   Frequency: How often should we track? Every frame, every 2nd frame, etc.
 */

#define ST_I18N "Filter.AutoFraming"

#define ST_I18N_TRACKING ST_I18N ".Tracking"
#define ST_KEY_TRACKING_MODE "Tracking.Mode"
#define ST_I18N_TRACKING_MODE ST_I18N_TRACKING ".Mode"
#define ST_I18N_FRAMING_MODE_SOLO ST_I18N_TRACKING_MODE ".Solo"
#define ST_I18N_FRAMING_MODE_GROUP ST_I18N_TRACKING_MODE ".Group"
#define ST_KEY_TRACKING_FREQUENCY "Tracking.Frequency"
#define ST_I18N_TRACKING_FREQUENCY ST_I18N_TRACKING ".Frequency"

#define ST_I18N_MOTION ST_I18N ".Motion"
#define ST_KEY_MOTION_PREDICTION "Motion.Prediction"
#define ST_I18N_MOTION_PREDICTION ST_I18N_MOTION ".Prediction"
#define ST_KEY_MOTION_SMOOTHING "Motion.Smoothing"
#define ST_I18N_MOTION_SMOOTHING ST_I18N_MOTION ".Smoothing"

#define ST_I18N_FRAMING ST_I18N ".Framing"
#define ST_KEY_FRAMING_STABILITY "Framing.Stability"
#define ST_I18N_FRAMING_STABILITY ST_I18N_FRAMING ".Stability"
#define ST_KEY_FRAMING_PADDING "Framing.Padding"
#define ST_I18N_FRAMING_PADDING ST_I18N_FRAMING ".Padding"
#define ST_KEY_FRAMING_OFFSET "Framing.Offset"
#define ST_I18N_FRAMING_OFFSET ST_I18N_FRAMING ".Offset"
#define ST_KEY_FRAMING_ASPECTRATIO "Framing.AspectRatio"
#define ST_I18N_FRAMING_ASPECTRATIO ST_I18N_FRAMING ".AspectRatio"

#define ST_KEY_ADVANCED_PROVIDER "Provider"
#define ST_I18N_ADVANCED_PROVIDER ST_I18N ".Provider"
#define ST_I18N_ADVANCED_PROVIDER_NVIDIA_FACEDETECTION ST_I18N_ADVANCED_PROVIDER ".NVIDIA.FaceDetection"

#define ST_KALMAN_EEC 1.0f

using streamfx::filter::autoframing::autoframing_factory;
using streamfx::filter::autoframing::autoframing_instance;
using streamfx::filter::autoframing::tracking_provider;

static constexpr std::string_view HELP_URL = "https://github.com/Xaymar/obs-StreamFX/wiki/Filter-Auto-Framing";

static tracking_provider provider_priority[] = {
	tracking_provider::NVIDIA_FACEDETECTION,
};

inline std::pair<bool, double_t> parse_text_as_size(const char* text)
{
	double_t v = 0;
	if (sscanf(text, "%lf", &v) == 1) {
		const char* prc_chr = strrchr(text, '%');
		if (prc_chr && (*prc_chr == '%')) {
			return {true, v / 100.0};
		} else {
			return {false, v};
		}
	} else {
		return {true, 1.0};
	}
}

const char* streamfx::filter::autoframing::cstring(tracking_provider provider)
{
	switch (provider) {
	case tracking_provider::INVALID:
		return "N/A";
	case tracking_provider::AUTOMATIC:
		return D_TRANSLATE(S_STATE_AUTOMATIC);
	case tracking_provider::NVIDIA_FACEDETECTION:
		return D_TRANSLATE(ST_I18N_ADVANCED_PROVIDER_NVIDIA_FACEDETECTION);
	default:
		throw std::runtime_error("Missing Conversion Entry");
	}
}

std::string streamfx::filter::autoframing::string(tracking_provider provider)
{
	return cstring(provider);
}

autoframing_instance::~autoframing_instance()
{
	D_LOG_DEBUG("Finalizing... (Addr: 0x%" PRIuPTR ")", this);

	{ // Unload the underlying effect ASAP.
		std::unique_lock<std::mutex> ul(_provider_lock);

		// De-queue the underlying task.
		if (_provider_task) {
			streamfx::threadpool()->pop(_provider_task);
			_provider_task->await_completion();
			_provider_task.reset();
		}

		// TODO: Make this asynchronous.
		switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
		case tracking_provider::NVIDIA_FACEDETECTION:
			nvar_facedetection_unload();
			break;
#endif
		default:
			break;
		}
	}
}

autoframing_instance::autoframing_instance(obs_data_t* data, obs_source_t* self)
	: source_instance(data, self),

	  _dirty(true), _size(1, 1), _out_size(1, 1),

	  _gfx_debug(), _standard_effect(), _input(), _vb(),

	  _provider(tracking_provider::INVALID), _provider_ui(tracking_provider::INVALID), _provider_ready(false),
	  _provider_lock(), _provider_task(),

	  _track_mode(tracking_mode::SOLO), _track_frequency(1),

	  _motion_smoothing(0.0), _motion_smoothing_kalman_pnc(1.), _motion_smoothing_kalman_mnc(1.),
	  _motion_prediction(0.0),

	  _frame_stability(0.), _frame_stability_kalman(1.), _frame_padding_prc(), _frame_padding(), _frame_offset_prc(),
	  _frame_offset(), _frame_aspect_ratio(0.0),

	  _track_frequency_counter(0), _tracked_elements(), _predicted_elements(),

	  _frame_pos_x({1., 1., 1., 1.}), _frame_pos_y({1., 1., 1., 1.}), _frame_pos({0, 0}), _frame_size({1, 1}),

	  _debug(false)
{
	D_LOG_DEBUG("Initializating... (Addr: 0x%" PRIuPTR ")", this);

	{
		::streamfx::obs::gs::context gctx;

		// Get debug renderer.
		_gfx_debug = ::streamfx::gfx::util::get();

		// Create the render target for the input buffering.
		_input = std::make_shared<::streamfx::obs::gs::rendertarget>(GS_RGBA_UNORM, GS_ZS_NONE);
		_input->render(1, 1); // Preallocate the RT on the driver and GPU.

		// Load the required effect.
		_standard_effect =
			std::make_shared<::streamfx::obs::gs::effect>(::streamfx::data_file_path("effects/standard.effect"));

		// Create the Vertex Buffer for rendering.
		_vb = std::make_shared<::streamfx::obs::gs::vertex_buffer>(uint32_t{4}, uint8_t{1});
		vec3_set(_vb->at(0).position, 0, 0, 0);
		vec3_set(_vb->at(1).position, 1, 0, 0);
		vec3_set(_vb->at(2).position, 0, 1, 0);
		vec3_set(_vb->at(3).position, 1, 1, 0);
		_vb->update(true);
	}

	if (data) {
		load(data);
	}
}

void autoframing_instance::load(obs_data_t* data)
{
	// Update from passed data.
	update(data);
}

void autoframing_instance::migrate(obs_data_t* data, uint64_t version)
{
	if (version < STREAMFX_MAKE_VERSION(0, 11, 0, 0)) {
		obs_data_unset_user_value(data, "ROI.Zoom");
		obs_data_unset_user_value(data, "ROI.Offset.X");
		obs_data_unset_user_value(data, "ROI.Offset.Y");
		obs_data_unset_user_value(data, "ROI.Stability");
	}
}

void autoframing_instance::update(obs_data_t* data)
{
	// Tracking
	_track_mode = static_cast<tracking_mode>(obs_data_get_int(data, ST_KEY_TRACKING_MODE));
	{
		if (const char* text = obs_data_get_string(data, ST_KEY_TRACKING_FREQUENCY); text != nullptr) {
			float value = 0.;
			if (sscanf(text, "%f", &value) == 1) {
				if (const char* seconds = strchr(text, 's'); seconds == nullptr) {
					value = 1.f / value; // Hz -> seconds
				} else {
					// No-op
				}
			}
			_track_frequency = value;
		}
	}
	_track_frequency_counter = 0;

	// Motion
	_motion_prediction           = static_cast<float>(obs_data_get_double(data, ST_KEY_MOTION_PREDICTION)) / 100.f;
	_motion_smoothing            = static_cast<float>(obs_data_get_double(data, ST_KEY_MOTION_SMOOTHING)) / 100.f;
	_motion_smoothing_kalman_pnc = streamfx::util::math::lerp<float>(1.0f, 0.00001f, _motion_smoothing);
	_motion_smoothing_kalman_mnc = streamfx::util::math::lerp<float>(0.001f, 1000.0f, _motion_smoothing);
	for (auto kv : _predicted_elements) {
		// Regenerate filters.
		kv.second->filter_pos_x  = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
									kv.second->filter_pos_x.get()};
		kv.second->filter_pos_y  = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
									kv.second->filter_pos_y.get()};
		kv.second->filter_size_x = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
									kv.second->filter_size_x.get()};
		kv.second->filter_size_y = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
									kv.second->filter_size_y.get()};
	}

	// Framing
	{ // Smoothing
		_frame_stability         = static_cast<float>(obs_data_get_double(data, ST_KEY_FRAMING_STABILITY)) / 100.f;
		_frame_stability_kalman  = streamfx::util::math::lerp<float>(1.0f, 0.00001f, _frame_stability);

		_frame_pos_x  = {_frame_stability_kalman, 1.0f, ST_KALMAN_EEC, _frame_pos_x.get()};
		_frame_pos_y  = {_frame_stability_kalman, 1.0f, ST_KALMAN_EEC, _frame_pos_y.get()};
		_frame_size_x = {_frame_stability_kalman, 1.0f, ST_KALMAN_EEC, _frame_size_x.get()};
		_frame_size_y = {_frame_stability_kalman, 1.0f, ST_KALMAN_EEC, _frame_size_y.get()};
	}
	{ // Padding
		if (const char* text = obs_data_get_string(data, ST_KEY_FRAMING_PADDING ".X"); text != nullptr) {
			float value = 0.;
			if (sscanf(text, "%f", &value) == 1) {
				if (const char* percent = strchr(text, '%'); percent != nullptr) {
					// Flip sign, percent is negative.
					value                 = -(value / 100.f);
					_frame_padding_prc[0] = true;
				} else {
					_frame_padding_prc[0] = false;
				}
			}
			_frame_padding.x = value;
		}
		if (const char* text = obs_data_get_string(data, ST_KEY_FRAMING_PADDING ".Y"); text != nullptr) {
			float value = 0.;
			if (sscanf(text, "%f", &value) == 1) {
				if (const char* percent = strchr(text, '%'); percent != nullptr) {
					// Flip sign, percent is negative.
					value                 = -(value / 100.f);
					_frame_padding_prc[1] = true;
				} else {
					_frame_padding_prc[1] = false;
				}
			}
			_frame_padding.y = value;
		}
	}
	{ // Offset
		if (const char* text = obs_data_get_string(data, ST_KEY_FRAMING_OFFSET ".X"); text != nullptr) {
			float value = 0.;
			if (sscanf(text, "%f", &value) == 1) {
				if (const char* percent = strchr(text, '%'); percent != nullptr) {
					// Flip sign, percent is negative.
					value                = -(value / 100.f);
					_frame_offset_prc[0] = true;
				} else {
					_frame_offset_prc[0] = false;
				}
			}
			_frame_offset.x = value;
		}
		if (const char* text = obs_data_get_string(data, ST_KEY_FRAMING_OFFSET ".Y"); text != nullptr) {
			float value = 0.;
			if (sscanf(text, "%f", &value) == 1) {
				if (const char* percent = strchr(text, '%'); percent != nullptr) {
					// Flip sign, percent is negative.
					value                = -(value / 100.f);
					_frame_offset_prc[1] = true;
				} else {
					_frame_offset_prc[1] = false;
				}
			}
			_frame_offset.y = value;
		}
	}
	{ // Aspect Ratio
		_frame_aspect_ratio = static_cast<float>(_size.first) / static_cast<float>(_size.second);
		if (const char* text = obs_data_get_string(data, ST_KEY_FRAMING_ASPECTRATIO); text != nullptr) {
			if (const char* percent = strchr(text, ':'); percent != nullptr) {
				float left  = 0.;
				float right = 0.;
				if ((sscanf(text, "%f", &left) == 1) && (sscanf(percent + 1, "%f", &right) == 1)) {
					_frame_aspect_ratio = left / right;
				} else {
					_frame_aspect_ratio = 0.0;
				}
			} else {
				float value = 0.;
				if (sscanf(text, "%f", &value) == 1) {
					_frame_aspect_ratio = value;
				} else {
					_frame_aspect_ratio = 0.0;
				}
			}
		}
	}

	// Advanced / Provider
	{ // Check if the user changed which Denoising provider we use.
		auto provider = static_cast<tracking_provider>(obs_data_get_int(data, ST_KEY_ADVANCED_PROVIDER));
		if (provider == tracking_provider::AUTOMATIC) {
			provider = autoframing_factory::get()->find_ideal_provider();
		}

		// Check if the provider was changed, and if so switch.
		if (provider != _provider) {
			_provider_ui = provider;
			switch_provider(provider);
		}

		if (_provider_ready) {
			std::unique_lock<std::mutex> ul(_provider_lock);

			switch (_provider) {
#ifdef ENABLE_FILTER_UPSCALING_NVIDIA
			case tracking_provider::NVIDIA_FACEDETECTION:
				nvar_facedetection_update();
				break;
#endif
			default:
				break;
			}
		}
	}

	_debug = obs_data_get_bool(data, "Debug");
}

void streamfx::filter::autoframing::autoframing_instance::properties(obs_properties_t* properties)
{
	switch (_provider_ui) {
#ifdef ENABLE_FILTER_AUTOFRAMING_NVIDIA
	case tracking_provider::NVIDIA_FACEDETECTION:
		nvar_facedetection_properties(properties);
		break;
#endif
	default:
		break;
	}
}

uint32_t autoframing_instance::get_width()
{
	if (_debug) {
		return std::max<uint32_t>(_size.first, 1);
	}
	return std::max<uint32_t>(_out_size.first, 1);
}

uint32_t autoframing_instance::get_height()
{
	if (_debug) {
		return std::max<uint32_t>(_size.second, 1);
	}
	return std::max<uint32_t>(_out_size.second, 1);
}

void autoframing_instance::video_tick(float_t seconds)
{
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	_size       = {width, height};

	{ // Calculate output size for aspect ratio.
		_out_size = _size;
		if (_frame_aspect_ratio > 0.0) {
			if (width > height) {
				_out_size.first =
					static_cast<uint32_t>(std::lroundf(static_cast<float>(_out_size.second) * _frame_aspect_ratio), 0,
										  std::numeric_limits<uint32_t>::max());
			} else {
				_out_size.second =
					static_cast<uint32_t>(std::lroundf(static_cast<float>(_out_size.first) * _frame_aspect_ratio), 0,
										  std::numeric_limits<uint32_t>::max());
			}
		}
	}

	// Update tracking.
	tracking_tick(seconds);

	// Mark the effect as dirty.
	_dirty = true;
}

void autoframing_instance::video_render(gs_effect_t* effect)
{
	auto parent = obs_filter_get_parent(_self);
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	vec4 blank  = vec4{0, 0, 0, 0};

	// Ensure we have the bare minimum of valid information.
	target = target ? target : parent;
	effect = effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);

	// Skip the filter if:
	// - The Provider isn't ready yet.
	// - We don't have a target.
	// - The width/height of the next filter in the chain is empty.
	if (!_provider_ready || !target || (width == 0) || (height == 0)) {
		obs_source_skip_video_filter(_self);
		return;
	}

#ifdef ENABLE_PROFILING
	::streamfx::obs::gs::debug_marker profiler0{::streamfx::obs::gs::debug_color_source, "StreamFX Auto-Framing"};
	::streamfx::obs::gs::debug_marker profiler0_0{::streamfx::obs::gs::debug_color_gray, "'%s' on '%s'",
												  obs_source_get_name(_self), obs_source_get_name(parent)};
#endif

	if (_dirty) {
		// Capture the input.
		if (obs_source_process_filter_begin(_self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
			auto op = _input->render(width, height);

			// Set correct projection matrix.
			gs_ortho(0, static_cast<float>(width), 0, static_cast<float>(height), 0, 1);

			// Clear the buffer
			gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &blank, 0, 0);

			// Set GPU state
			gs_blend_state_push();
			gs_enable_color(true, true, true, true);
			gs_enable_blending(false);
			gs_enable_depth_test(false);
			gs_enable_stencil_test(false);
			gs_set_cull_mode(GS_NEITHER);

			// Render
			bool srgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(gs_get_linear_srgb());
			obs_source_process_filter_end(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height);
			gs_enable_framebuffer_srgb(srgb);

			// Reset GPU state
			gs_blend_state_pop();
		} else {
			obs_source_skip_video_filter(_self);
			return;
		}

		// Lock & Process the captured input with the provider.
		if (_track_frequency_counter >= _track_frequency) {
			_track_frequency_counter = 0;

			std::unique_lock<std::mutex> ul(_provider_lock);
			switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
			case tracking_provider::NVIDIA_FACEDETECTION:
				nvar_facedetection_process();
				break;
#endif
			default:
				obs_source_skip_video_filter(_self);
				return;
			}
		}

		_dirty = false;
	}

	{ // Draw the result for the next filter to use.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_render, "Render"};
#endif

		if (_debug) { // Debug Mode
			gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), _input->get_object());
			while (gs_effect_loop(effect, "Draw")) {
				gs_draw_sprite(nullptr, 0, _size.first, _size.second);
			}

			int index = 0;
			for (auto kv : _predicted_elements) {
				index++;
				float x_indicator_spacing = kv.first->size.x / 8.f;
				float y_indicator_height  = kv.first->size.y / 5.f;

				// Tracked Area (Red)
				_gfx_debug->draw_rectangle(kv.first->pos.x - kv.first->size.x / 2.f,
										   kv.first->pos.y - kv.first->size.y / 2.f, kv.first->size.x, kv.first->size.y,
										   true, 0x7E0000FF);
				{
					float x = kv.first->pos.x - kv.first->size.x / 2.f;
					float y = kv.first->pos.y - kv.first->size.y / 2.f;
					// Draw index indicator
					for (int i = 0; i < index; i++) {
						float xPos = x + (float)i * x_indicator_spacing;
						_gfx_debug->draw_line(xPos, y, xPos, y - y_indicator_height, 0xDE0000FF);
					}
					// Draw confidence line
					_gfx_debug->draw_line(x, y, x + kv.first->confidence * kv.first->size.x, y, 0xFFFFFFFF);
				}

				// Velocity Arrow (Black)
				_gfx_debug->draw_arrow(kv.first->pos.x, kv.first->pos.y, kv.first->pos.x + kv.first->vel.x,
									   kv.first->pos.y + kv.first->vel.y, 0., 0x7E000000);

					// Predicted Area (Orange)
					_gfx_debug->draw_rectangle(kv.second->mp_pos.x - kv.first->size.x / 2.f,
											   kv.second->mp_pos.y - kv.first->size.y / 2.f, kv.first->size.x,
											   kv.first->size.y, true, 0x7E007EFF);

					// Filtered Area (Yellow)
					_gfx_debug->draw_rectangle(kv.second->filter_pos_x.get() - kv.second->filter_size_x.get() / 2.f,
											   kv.second->filter_pos_y.get() - kv.second->filter_size_y.get() / 2.f,
											   kv.second->filter_size_x.get(),
											   kv.second->filter_size_y.get(),
											   true, 0x7E00FFFF);
					{
						float x = kv.second->filter_pos_x.get() - kv.second->filter_size_x.get() / 2.f;
						float y = kv.second->filter_pos_y.get() - kv.second->filter_size_y.get() / 2.f;
						// Draw index indicator
						for (int i = 0; i < index; i++) {
							float xPos = x + (float)i * x_indicator_spacing;
							_gfx_debug->draw_line(xPos, y, xPos, y - y_indicator_height, 0xDE00FFFF);
						}
					}

					// Offset Filtered Area (Blue)
					_gfx_debug->draw_rectangle(kv.second->offset_pos.x - kv.second->filter_size_x.get() / 2.f,
											   kv.second->offset_pos.y - kv.second->filter_size_y.get() / 2.f,
											   kv.second->filter_size_x.get(),
											   kv.second->filter_size_y.get(),
											   true, 0x7EFF0000);

					// Padded Offset Filtered Area (Cyan)
					_gfx_debug->draw_rectangle(kv.second->offset_pos.x - kv.second->pad_size.x / 2.f,
											   kv.second->offset_pos.y - kv.second->pad_size.y / 2.f, kv.second->pad_size.x,
											   kv.second->pad_size.y, true, 0x7EFFFF00);

					// Aspect-Ratio-Corrected Padded Offset Filtered Area (Green)
					_gfx_debug->draw_rectangle(kv.second->offset_pos.x - kv.second->aspected_size.x / 2.f,
											   kv.second->offset_pos.y - kv.second->aspected_size.y / 2.f,
											   kv.second->aspected_size.x, kv.second->aspected_size.y, true, 0x7E00FF00);
				}

			// Final Region (White)
			_gfx_debug->draw_rectangle(_frame_pos.x - _frame_size.x / 2.f, _frame_pos.y - _frame_size.y / 2.f,
									   _frame_size.x, _frame_size.y, true, 0x7EFFFFFF);
		} else {
			float x0 = (_frame_pos.x - _frame_size.x / 2.f) / static_cast<float>(_size.first);
			float x1 = (_frame_pos.x + _frame_size.x / 2.f) / static_cast<float>(_size.first);
			float y0 = (_frame_pos.y - _frame_size.y / 2.f) / static_cast<float>(_size.second);
			float y1 = (_frame_pos.y + _frame_size.y / 2.f) / static_cast<float>(_size.second);

			{
				auto v = _vb->at(0);
				vec3_set(v.position, 0., 0., 0.);
				v.uv[0]->x = x0;
				v.uv[0]->y = y0;
			}
			{
				auto v = _vb->at(1);
				vec3_set(v.position, static_cast<float>(_out_size.first), 0., 0.);
				v.uv[0]->x = x1;
				v.uv[0]->y = y0;
			}
			{
				auto v = _vb->at(2);
				vec3_set(v.position, 0., static_cast<float>(_out_size.second), 0.);
				v.uv[0]->x = x0;
				v.uv[0]->y = y1;
			}
			{
				auto v = _vb->at(3);
				vec3_set(v.position, static_cast<float>(_out_size.first), static_cast<float>(_out_size.second), 0.);
				v.uv[0]->x = x1;
				v.uv[0]->y = y1;
			}

			gs_load_vertexbuffer(_vb->update(true));
			if (!effect) {
				if (_standard_effect->has_parameter("InputA", ::streamfx::obs::gs::effect_parameter::type::Texture)) {
					_standard_effect->get_parameter("InputA").set_texture(_input->get_texture());
				}

				while (gs_effect_loop(_standard_effect->get_object(), "Texture")) {
					gs_draw(GS_TRISTRIP, 0, 4);
				}
			} else {
				gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"),
									  _input->get_texture()->get_object());

				while (gs_effect_loop(effect, "Draw")) {
					gs_draw(GS_TRISTRIP, 0, 4);
				}
			}
			gs_load_vertexbuffer(nullptr);
		}
	}
}

void streamfx::filter::autoframing::autoframing_instance::tracking_tick(float seconds)
{
	{ // Increase the age of all elements, and kill off any that are "too old".
		float threshold = (0.5f * (1.f / (1.f - _track_frequency)));

		auto iter = _tracked_elements.begin();
		while (iter != _tracked_elements.end()) {
			// Increment the age by the tick duration.
			(*iter)->age += seconds;

			// If the age exceeds the threshold, remove it.
			if ((*iter)->age >= threshold) {
				if (iter == _tracked_elements.begin()) {
					// Erase iter, then reset to start.
					_predicted_elements.erase(*iter);
					_tracked_elements.erase(iter);
					iter = _tracked_elements.begin();
				} else {
					// Copy, then advance before erasing.
					auto iter2 = iter;
					iter++;
					_predicted_elements.erase(*iter2);
					_tracked_elements.erase(iter2);
				}
			} else {
				// Move ahead.
				iter++;
			}
		}
	}

	for (auto trck : _tracked_elements) { // Updated predicted elements
		std::shared_ptr<pred_el> pred;

		// Find the corresponding prediction element.
		auto iter = _predicted_elements.find(trck);
		if (iter == _predicted_elements.end()) {
			pred = std::make_shared<pred_el>();
			_predicted_elements.insert_or_assign(trck, pred);
			pred->filter_pos_x  = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
								   trck->pos.x};
			pred->filter_pos_y  = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
								   trck->pos.y};
			pred->filter_size_x = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
								   trck->size.x};
			pred->filter_size_y = {_motion_smoothing_kalman_pnc, _motion_smoothing_kalman_mnc, ST_KALMAN_EEC,
								   trck->size.y};

		} else {
			pred = iter->second;
		}

		// Calculate absolute velocity.
		vec2 vel;
		vec2_copy(&vel, &trck->vel);
		vec2_mulf(&vel, &vel, _motion_prediction);
		vec2_mulf(&vel, &vel, seconds);

		// Calculate predicted position.
		vec2 pos;
		if (trck->age > seconds) {
			vec2_copy(&pos, &pred->mp_pos);
		} else {
			vec2_copy(&pos, &trck->pos);
		}
		vec2_add(&pos, &pos, &vel);
		vec2_copy(&pred->mp_pos, &pos);

		// Update filtered position.
		pred->filter_pos_x.filter(pred->mp_pos.x);
		pred->filter_pos_y.filter(pred->mp_pos.y);
		pred->filter_size_x.filter(trck->size.x);
		pred->filter_size_y.filter(trck->size.y);

		// Update offset position.
		vec2_set(&pred->offset_pos, pred->filter_pos_x.get(), pred->filter_pos_y.get());
		if (_frame_offset_prc[0]) { // %
			pred->offset_pos.x += pred->filter_size_x.get() * (-_frame_offset.x);
		} else { // Pixels
			pred->offset_pos.x += _frame_offset.x;
		}
		if (_frame_offset_prc[1]) { // %
			pred->offset_pos.y += pred->filter_size_y.get() * (-_frame_offset.y);
		} else { // Pixels
			pred->offset_pos.y += _frame_offset.y;
		}

		// Calculate padded area.
		vec2_copy(&pred->pad_size, &trck->size);
		if (_frame_padding_prc[0]) { // %
			pred->pad_size.x += pred->filter_size_x.get() * (-_frame_padding.x) * 2.f;
		} else { // Pixels
			pred->pad_size.x += _frame_padding.x * 2.f;
		}
		if (_frame_padding_prc[1]) { // %
			pred->pad_size.y += pred->filter_size_y.get() * (-_frame_padding.y) * 2.f;
		} else { // Pixels
			pred->pad_size.y += _frame_padding.y * 2.f;
		}

		// Adjust to match aspect ratio (width / height).
		vec2_copy(&pred->aspected_size, &pred->pad_size);
		if (_frame_aspect_ratio > 0.0) {
			if ((pred->aspected_size.x / pred->aspected_size.y) >= _frame_aspect_ratio) { // Ours > Target
				pred->aspected_size.y = pred->aspected_size.x / _frame_aspect_ratio;
			} else { // Target > Ours
				pred->aspected_size.x = pred->aspected_size.y * _frame_aspect_ratio;
			}
		}
	}

	{ // Find final frame.
		bool need_filter = true;
		if (_predicted_elements.size() > 0) {
			if (_track_mode == tracking_mode::SOLO) {
				auto kv = _predicted_elements.rbegin();

				_frame_pos_x.filter(kv->second->offset_pos.x);
				_frame_pos_y.filter(kv->second->offset_pos.y);

				vec2_set(&_frame_pos, _frame_pos_x.get(), _frame_pos_y.get());
				vec2_copy(&_frame_size, &kv->second->aspected_size);

				need_filter = false;
			} else {
				vec2 min;
				vec2 max;

				vec2_set(&min, std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
				vec2_set(&max, 0., 0.);

				for (auto kv : _predicted_elements) {
					vec2 size;
					vec2 low;
					vec2 high;

					vec2_copy(&size, &kv.second->aspected_size);
					vec2_mulf(&size, &size, .5f);

					vec2_copy(&low, &kv.second->offset_pos);
					vec2_copy(&high, &kv.second->offset_pos);

					vec2_sub(&low, &low, &size);
					vec2_add(&high, &high, &size);

					if (low.x < min.x) {
						min.x = low.x;
					}
					if (low.y < min.y) {
						min.y = low.y;
					}
					if (high.x > max.x) {
						max.x = high.x;
					}
					if (high.y > max.y) {
						max.y = high.y;
					}
				}

				// Calculate center.
				vec2 center;
				vec2_add(&center, &min, &max);
				vec2_divf(&center, &center, 2.f);

				// Assign center.
				_frame_pos_x.filter(center.x);
				_frame_pos_y.filter(center.y);

				// Calculate size.
				vec2 size;
				vec2_copy(&size, &max);
				vec2_sub(&size, &size, &min);
				_frame_size_x.filter(size.x);
				_frame_size_y.filter(size.y);
			}
		} else {
			_frame_pos_x.filter(static_cast<float>(_size.first) / 2.f);
			_frame_pos_y.filter(static_cast<float>(_size.second) / 2.f);
			_frame_size_x.filter(static_cast<float>(_size.first));
			_frame_size_y.filter(static_cast<float>(_size.second));
		}

		// Grab filtered data if needed, otherwise stick with direct data.
		if (need_filter) {
			vec2_set(&_frame_pos, _frame_pos_x.get(), _frame_pos_y.get());
			vec2_set(&_frame_size, _frame_size_x.get(), _frame_size_y.get());
		}

		{ // Aspect Ratio correction is a three step process:
			float aspect = _frame_aspect_ratio > 0.
							   ? _frame_aspect_ratio
							   : (static_cast<float>(_size.first) / static_cast<float>(_size.second));

			{ // 1. Adjust aspect ratio so that all elements end up contained.
				float frame_aspect = _frame_size.x / _frame_size.y;
				if (aspect < frame_aspect) {
					_frame_size.y = _frame_size.x / aspect;
				} else {
					_frame_size.x = _frame_size.y * aspect;
				}
			}

			// 2. Limit the size of the frame to the allowed region, and adjust it so it's inside the frame.
			// This will move the center, which might not be a wanted side effect.
			vec4 rect;
			rect.x       = std::clamp<float>(_frame_pos.x - _frame_size.x / 2.f, 0.f, static_cast<float>(_size.first));
			rect.z       = std::clamp<float>(_frame_pos.x + _frame_size.x / 2.f, 0.f, static_cast<float>(_size.first));
			rect.y       = std::clamp<float>(_frame_pos.y - _frame_size.y / 2.f, 0.f, static_cast<float>(_size.second));
			rect.w       = std::clamp<float>(_frame_pos.y + _frame_size.y / 2.f, 0.f, static_cast<float>(_size.second));
			_frame_pos.x = (rect.x + rect.z) / 2.f;
			_frame_pos.y = (rect.y + rect.w) / 2.f;
			_frame_size.x = (rect.z - rect.x);
			_frame_size.y = (rect.w - rect.y);

			{ // 3. Adjust the aspect ratio so that it matches the expected output aspect ratio.
				float frame_aspect = _frame_size.x / _frame_size.y;
				if (aspect < frame_aspect) {
					_frame_size.x = _frame_size.y * aspect;
				} else {
					_frame_size.y = _frame_size.x / aspect;
				}
			}
		}
	}

	// Increment tracking counter.
	_track_frequency_counter += seconds;
}

struct switch_provider_data_t {
	tracking_provider provider;
};

void streamfx::filter::autoframing::autoframing_instance::switch_provider(tracking_provider provider)
{
	std::unique_lock<std::mutex> ul(_provider_lock);

	// Safeguard against calls made from unlocked memory.
	if (provider == _provider) {
		return;
	}

	// This doesn't work correctly.
	// - Need to allow multiple switches at once because OBS is weird.
	// - Doesn't guarantee that the task is properly killed off.

	// Log information.
	D_LOG_INFO("Instance '%s' is switching provider from '%s' to '%s'.", obs_source_get_name(_self), cstring(_provider),
			   cstring(provider));

	// If there is an ongoing task to switch provider, cancel it.
	if (_provider_task) {
		// De-queue it.
		streamfx::threadpool()->pop(_provider_task);

		// Await the death of the task itself.
		_provider_task->await_completion();

		// Clear any memory associated with it.
		_provider_task.reset();
	}

	// Build data to pass into the task.
	auto spd      = std::make_shared<switch_provider_data_t>();
	spd->provider = _provider;
	_provider     = provider;

	// Then spawn a new task to switch provider.
	_provider_task = streamfx::threadpool()->push(
		std::bind(&autoframing_instance::task_switch_provider, this, std::placeholders::_1), spd);
}

void streamfx::filter::autoframing::autoframing_instance::task_switch_provider(util::threadpool::task_data_t data)
{
	std::shared_ptr<switch_provider_data_t> spd = std::static_pointer_cast<switch_provider_data_t>(data);

	// Mark the provider as no longer ready.
	_provider_ready = false;

	// Lock the provider from being used.
	std::unique_lock<std::mutex> ul(_provider_lock);

	try {
		// Unload the previous provider.
		switch (spd->provider) {
#ifdef ENABLE_FILTER_AUTOFRAMING_NVIDIA
		case tracking_provider::NVIDIA_FACEDETECTION:
			nvar_facedetection_unload();
			break;
#endif
		default:
			break;
		}

		// Load the new provider.
		switch (_provider) {
#ifdef ENABLE_FILTER_AUTOFRAMING_NVIDIA
		case tracking_provider::NVIDIA_FACEDETECTION:
			nvar_facedetection_load();
			break;
#endif
		default:
			break;
		}

		// Log information.
		D_LOG_INFO("Instance '%s' switched provider from '%s' to '%s'.", obs_source_get_name(_self),
				   cstring(spd->provider), cstring(_provider));

		_provider_ready = true;
	} catch (std::exception const& ex) {
		// Log information.
		D_LOG_ERROR("Instance '%s' failed switching provider with error: %s", obs_source_get_name(_self), ex.what());
	}
}

#ifdef ENABLE_FILTER_AUTOFRAMING_NVIDIA
void streamfx::filter::autoframing::autoframing_instance::nvar_facedetection_load()
{
	_nvidia_fx = std::make_shared<::streamfx::nvidia::ar::facedetection>();
	nvar_facedetection_update();
}

void streamfx::filter::autoframing::autoframing_instance::nvar_facedetection_unload()
{
	_nvidia_fx.reset();
}

void streamfx::filter::autoframing::autoframing_instance::nvar_facedetection_process()
{
	if (!_nvidia_fx) {
		return;
	}

	// Frames may not move more than this distance.
	float max_dst =
		sqrtf(static_cast<float>(_size.first * _size.first) + static_cast<float>(_size.second * _size.second)) * 0.667f;
	max_dst *= 1.f / (1.f - _track_frequency); // Fine-tune this?

	// Process the current frame (if requested).
	_nvidia_fx->process(_input->get_texture());

	// If there are tracked faces, merge them with the tracked elements.
	if (auto edx = _nvidia_fx->count(); edx > 0) {
		std::list<std::shared_ptr<track_el>> boxes;

		for (size_t idx = 0; idx < edx; idx++) {
			float confidence = 0.;
			auto  rect       = _nvidia_fx->at(idx, confidence);

			// Skip elements that have not enough confidence of being a face.
			// TODO: Make the threshold configurable.
			if (confidence < .5) {
				continue;
			}

			// Calculate centered position.
			vec2 pos;
			pos.x = rect.x + (rect.z / 2.f);
			pos.y = rect.y + (rect.w / 2.f);

			// Create potential match
			auto match = std::make_shared<track_el>();
			vec2_copy(&match->pos, &pos);
			vec2_set(&match->size, rect.z, rect.w);
			vec2_set(&match->vel, 0., 0.);
			match->age = 0.;
			match->confidence = confidence > 1.f ? 1.f : confidence; // confidence values go above 1 in SOLO mode.

			boxes.push_back(match);
		}

		for (const auto& el : _tracked_elements) {
			// Search for matches for existing tracked elements
			auto match_iter = boxes.end();
			float match_dst = max_dst;

			auto iter = boxes.begin();
			while (iter != boxes.end()) {
				auto box = (*iter);

				// Check if the distance is within acceptable bounds.
				float dst = vec2_dist(&box->pos, &el->pos);
				if ((dst < match_dst) && (dst < max_dst)) {
					match_dst  = dst;
					match_iter = iter;
				}

				iter++;
			}

			// If a match was found
			if (match_iter != boxes.end()) {
				auto match = *match_iter;

				// Calculate the velocity between changes.
				vec2 vel;
				vec2_sub(&vel, &el->pos, &match->pos);

				// Update information.
				vec2_copy(&el->pos, &match->pos);
				vec2_copy(&el->size, &match->size);
				vec2_copy(&el->vel, &vel);
					el->age = 0.;
				el->confidence = match->confidence;

				boxes.erase(match_iter);
			}
		}

		// Add new tracked elements for each remaining unmatched box
		for (auto box : boxes) {
			_tracked_elements.push_back(box);
		}
	}
}

void streamfx::filter::autoframing::autoframing_instance::nvar_facedetection_properties(obs_properties_t* props) {}

void streamfx::filter::autoframing::autoframing_instance::nvar_facedetection_update()
{
	if (!_nvidia_fx) {
		return;
	}

	switch (_track_mode) {
	case tracking_mode::SOLO:
		_nvidia_fx->set_tracking_limit(1);
		if (!_nvidia_fx->is_temporal()) {
			_track_mode = tracking_mode::GROUP;
		}
		break;
	case tracking_mode::GROUP:
		_nvidia_fx->set_tracking_limit(_nvidia_fx->tracking_limit_range().second);
		break;
	}
}

#endif

autoframing_factory::autoframing_factory()
{
	bool any_available = false;

	// 1. Try and load any configured providers.
#ifdef ENABLE_FILTER_AUTOFRAMING_NVIDIA
	try {
		// Load CVImage and Video Effects SDK.
		_nvcuda           = ::streamfx::nvidia::cuda::obs::get();
		_nvcvi            = ::streamfx::nvidia::cv::cv::get();
		_nvar             = ::streamfx::nvidia::ar::ar::get();
		_nvidia_available = true;
		any_available |= _nvidia_available;
	} catch (const std::exception& ex) {
		_nvidia_available = false;
		_nvar.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA providers available due to error: %s", ex.what());
	} catch (...) {
		_nvidia_available = false;
		_nvar.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA providers available with unknown error.", nullptr);
	}
#endif

	// 2. Check if any of them managed to load at all.
	if (!any_available) {
		D_LOG_ERROR("All supported providers failed to initialize, disabling effect.", 0);
		return;
	}

	// Register initial source.
	_info.id           = S_PREFIX "filter-autoframing";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO;

	support_size(true);
	finish_setup();

	// Register proxy identifiers.
	register_proxy("streamfx-filter-nvidia-face-tracking");
	register_proxy("streamfx-nvidia-face-tracking");
}

autoframing_factory::~autoframing_factory() {}

const char* autoframing_factory::get_name()
{
	return D_TRANSLATE(ST_I18N);
}

void autoframing_factory::get_defaults2(obs_data_t* data)
{
	// Tracking
	obs_data_set_default_int(data, ST_KEY_TRACKING_MODE, static_cast<int64_t>(tracking_mode::SOLO));
	obs_data_set_default_string(data, ST_KEY_TRACKING_FREQUENCY, "20 Hz");

	// Motion
	obs_data_set_default_double(data, ST_KEY_MOTION_SMOOTHING, 33.333);
	obs_data_set_default_double(data, ST_KEY_MOTION_PREDICTION, 200.0);

	// Framing
	obs_data_set_default_double(data, ST_KEY_FRAMING_STABILITY, 10.0);
	obs_data_set_default_string(data, ST_KEY_FRAMING_PADDING ".X", "33.333 %");
	obs_data_set_default_string(data, ST_KEY_FRAMING_PADDING ".Y", "33.333 %");
	obs_data_set_default_string(data, ST_KEY_FRAMING_OFFSET ".X", " 0.00 %");
	obs_data_set_default_string(data, ST_KEY_FRAMING_OFFSET ".Y", "-7.50 %");
	obs_data_set_default_string(data, ST_KEY_FRAMING_ASPECTRATIO, "");

	// Advanced
	obs_data_set_default_int(data, ST_KEY_ADVANCED_PROVIDER, static_cast<int64_t>(tracking_provider::AUTOMATIC));
	obs_data_set_default_bool(data, "Debug", false);
}

static bool modified_provider(obs_properties_t* props, obs_property_t*, obs_data_t* settings) noexcept
{
	try {
		return true;
	} catch (const std::exception& ex) {
		DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
		return false;
	} catch (...) {
		DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
		return false;
	}
}

obs_properties_t* autoframing_factory::get_properties2(autoframing_instance* data)
{
	obs_properties_t* pr = obs_properties_create();

#ifdef ENABLE_FRONTEND
	{
		obs_properties_add_button2(pr, S_MANUAL_OPEN, D_TRANSLATE(S_MANUAL_OPEN), autoframing_factory::on_manual_open,
								   nullptr);
	}
#endif

	{
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, ST_I18N_TRACKING, D_TRANSLATE(ST_I18N_TRACKING), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_list(grp, ST_KEY_TRACKING_MODE, D_TRANSLATE(ST_I18N_TRACKING_MODE),
											 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			obs_property_set_modified_callback(p, modified_provider);
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_FRAMING_MODE_SOLO),
									  static_cast<int64_t>(tracking_mode::SOLO));
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_FRAMING_MODE_GROUP),
									  static_cast<int64_t>(tracking_mode::GROUP));
		}

		{
			auto p = obs_properties_add_text(grp, ST_KEY_TRACKING_FREQUENCY, D_TRANSLATE(ST_I18N_TRACKING_FREQUENCY),
											 OBS_TEXT_DEFAULT);
		}
		}

	{
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, ST_I18N_MOTION, D_TRANSLATE(ST_I18N_MOTION), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_float_slider(grp, ST_KEY_MOTION_SMOOTHING,
													 D_TRANSLATE(ST_I18N_MOTION_SMOOTHING), 0.0, 100.0, 0.01);
			obs_property_float_set_suffix(p, " %");
		}

		{
			auto p = obs_properties_add_float_slider(grp, ST_KEY_MOTION_PREDICTION,
													 D_TRANSLATE(ST_I18N_MOTION_PREDICTION), 0.0, 500.0, 0.01);
			obs_property_float_set_suffix(p, " %");
		}
	}

	{
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, ST_I18N_FRAMING, D_TRANSLATE(ST_I18N_FRAMING), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_float_slider(grp, ST_KEY_FRAMING_STABILITY,
													 D_TRANSLATE(ST_I18N_FRAMING_STABILITY), 0.0, 100.0, 0.01);
			obs_property_float_set_suffix(p, " %");
		}

		{
			auto grp2 = obs_properties_create();
			obs_properties_add_group(grp, ST_KEY_FRAMING_PADDING, D_TRANSLATE(ST_I18N_FRAMING_PADDING),
									 OBS_GROUP_NORMAL, grp2);

			{
				auto p = obs_properties_add_text(grp2, ST_KEY_FRAMING_PADDING ".X", "X", OBS_TEXT_DEFAULT);
			}
			{
				auto p = obs_properties_add_text(grp2, ST_KEY_FRAMING_PADDING ".Y", "Y", OBS_TEXT_DEFAULT);
			}
		}

		{
			auto grp2 = obs_properties_create();
			obs_properties_add_group(grp, ST_KEY_FRAMING_OFFSET, D_TRANSLATE(ST_I18N_FRAMING_OFFSET), OBS_GROUP_NORMAL,
									 grp2);

			{
				auto p = obs_properties_add_text(grp2, ST_KEY_FRAMING_OFFSET ".X", "X", OBS_TEXT_DEFAULT);
			}
			{
				auto p = obs_properties_add_text(grp2, ST_KEY_FRAMING_OFFSET ".Y", "Y", OBS_TEXT_DEFAULT);
			}
		}

		{
			auto p = obs_properties_add_list(grp, ST_KEY_FRAMING_ASPECTRATIO, D_TRANSLATE(ST_I18N_FRAMING_ASPECTRATIO),
											 OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
			obs_property_list_add_string(p, "None", "");
			obs_property_list_add_string(p, "1:1", "1:1");

			obs_property_list_add_string(p, "3:2", "3:2");
			obs_property_list_add_string(p, "2:3", "2:3");

			obs_property_list_add_string(p, "4:3", "4:3");
			obs_property_list_add_string(p, "3:4", "3:4");

			obs_property_list_add_string(p, "5:4", "5:4");
			obs_property_list_add_string(p, "4:5", "4:5");

			obs_property_list_add_string(p, "16:9", "16:9");
			obs_property_list_add_string(p, "9:16", "9:16");

			obs_property_list_add_string(p, "16:10", "16:10");
			obs_property_list_add_string(p, "10:16", "10:16");

			obs_property_list_add_string(p, "21:9", "21:9");
			obs_property_list_add_string(p, "9:21", "9:21");

			obs_property_list_add_string(p, "21:10", "21:10");
			obs_property_list_add_string(p, "10:21", "10:21");

			obs_property_list_add_string(p, "32:9", "32:9");
			obs_property_list_add_string(p, "9:32", "9:32");

			obs_property_list_add_string(p, "32:10", "32:10");
			obs_property_list_add_string(p, "10:32", "10:32");
		}
	}

	if (data) {
		data->properties(pr);
	}

	{ // Advanced Settings
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, S_ADVANCED, D_TRANSLATE(S_ADVANCED), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_list(grp, ST_KEY_ADVANCED_PROVIDER, D_TRANSLATE(ST_I18N_ADVANCED_PROVIDER),
											 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			obs_property_set_modified_callback(p, modified_provider);
			obs_property_list_add_int(p, D_TRANSLATE(S_STATE_AUTOMATIC),
									  static_cast<int64_t>(tracking_provider::AUTOMATIC));
#ifdef ENABLE_FILTER_AUTOFRAMING_NVIDIA
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_ADVANCED_PROVIDER_NVIDIA_FACEDETECTION),
									  static_cast<int64_t>(tracking_provider::NVIDIA_FACEDETECTION));
#endif
		}

		obs_properties_add_bool(grp, "Debug", "Debug");
	}

	return pr;
}

#ifdef ENABLE_FRONTEND
bool streamfx::filter::autoframing::autoframing_factory::on_manual_open(obs_properties_t* props,
																		obs_property_t* property, void* data)
{
	streamfx::open_url(HELP_URL);
	return false;
}
#endif

bool streamfx::filter::autoframing::autoframing_factory::is_provider_available(tracking_provider provider)
{
	switch (provider) {
#ifdef ENABLE_FILTER_AUTOFRAMING_NVIDIA
	case tracking_provider::NVIDIA_FACEDETECTION:
		return _nvidia_available;
#endif
	default:
		return false;
	}
}

tracking_provider streamfx::filter::autoframing::autoframing_factory::find_ideal_provider()
{
	for (auto v : provider_priority) {
		if (is_provider_available(v)) {
			return v;
			break;
		}
	}
	return tracking_provider::INVALID;
}

std::shared_ptr<autoframing_factory> _filter_autoframing_factory_instance = nullptr;

void autoframing_factory::initialize()
{
	try {
		if (!_filter_autoframing_factory_instance)
			_filter_autoframing_factory_instance = std::make_shared<autoframing_factory>();
	} catch (const std::exception& ex) {
		D_LOG_ERROR("Failed to initialize due to error: %s", ex.what());
	} catch (...) {
		D_LOG_ERROR("Failed to initialize due to unknown error.", "");
	}
}

void autoframing_factory::finalize()
{
	_filter_autoframing_factory_instance.reset();
}

std::shared_ptr<autoframing_factory> autoframing_factory::get()
{
	return _filter_autoframing_factory_instance;
}
