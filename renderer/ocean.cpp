/* Copyright (c) 2017-2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "fft/glfft_granite_interface.hpp"
#include "ocean.hpp"
#include "device.hpp"
#include "renderer.hpp"
#include "render_context.hpp"
#include "render_graph.hpp"
#include "muglm/matrix_helper.hpp"

using namespace std;

namespace Granite
{
static constexpr unsigned MaxLODIndirect = 8;

struct OceanVertex
{
	uint8_t pos[4];
	uint8_t weights[4];
};

Ocean::Ocean()
{
	EVENT_MANAGER_REGISTER_LATCH(Ocean, on_device_created, on_device_destroyed, Vulkan::DeviceCreatedEvent);
	EVENT_MANAGER_REGISTER(Ocean, on_frame_tick, FrameTickEvent);
}

Ocean::Handles Ocean::add_to_scene(Scene &scene)
{
	Handles handles;
	handles.entity = scene.create_entity();

	auto ocean = Util::make_handle<Ocean>();

	auto *update_component = handles.entity->allocate_component<PerFrameUpdateComponent>();
	update_component->refresh = ocean.get();

	auto *rp = handles.entity->allocate_component<RenderPassComponent>();
	rp->creator = ocean.get();

	auto *renderable = handles.entity->allocate_component<RenderableComponent>();
	renderable->renderable = ocean;

	handles.entity->allocate_component<OpaqueComponent>();
	handles.entity->allocate_component<UnboundedComponent>();

	return handles;
}

bool Ocean::on_frame_tick(const Granite::FrameTickEvent &e)
{
	current_time = e.get_elapsed_time();
	return true;
}

void Ocean::on_device_created(const Vulkan::DeviceCreatedEvent &e)
{
	fft_iface = FFTInterface(&e.get_device());
	GLFFT::FFTOptions options = {};
	options.type.fp16 = true;
	options.type.input_fp16 = true;
	options.type.output_fp16 = true;
#if 0
	options.performance.shared_banked = true;
	options.performance.workgroup_size_x = 64;
	options.performance.workgroup_size_y = 1;
	options.performance.vector_size = 2;
#endif

	auto cache = make_shared<GLFFT::ProgramCache>();

	height_fft.reset(new GLFFT::FFT(&fft_iface, height_fft_size, height_fft_size,
	                                GLFFT::ComplexToReal, GLFFT::Inverse,
	                                GLFFT::SSBO, GLFFT::ImageReal,
	                                cache, options));

	displacement_fft.reset(new GLFFT::FFT(&fft_iface, displacement_fft_size, displacement_fft_size,
	                                      GLFFT::ComplexToComplex, GLFFT::Inverse,
	                                      GLFFT::SSBO, GLFFT::Image,
	                                      cache, options));

	normal_fft.reset(new GLFFT::FFT(&fft_iface, normal_fft_size, normal_fft_size,
	                                GLFFT::ComplexToComplex, GLFFT::Inverse,
	                                GLFFT::SSBO, GLFFT::Image,
	                                cache, options));

	build_buffers(e.get_device());
	init_distributions(e.get_device());
}

void Ocean::on_device_destroyed(const Vulkan::DeviceCreatedEvent &)
{
	vertex_mip_views.clear();
	fragment_mip_views.clear();
	normal_mip_views.clear();

	height_fft.reset();
	normal_fft.reset();
	displacement_fft.reset();
	distribution_buffer.reset();
	distribution_buffer_displacement.reset();
	distribution_buffer_normal.reset();

	quad_lod.clear();
}

void Ocean::refresh(RenderContext &context)
{
	last_camera_position = context.get_render_parameters().camera_position;
}

void Ocean::set_base_renderer(Renderer *,
                              Renderer *,
                              Renderer *)
{
}

void Ocean::set_base_render_context(const RenderContext *context)
{
	this->context = context;
}

void Ocean::set_scene(Scene *)
{
}

void Ocean::setup_render_pass_dependencies(RenderGraph &, RenderPass &target)
{
	target.add_indirect_buffer_input("ocean-lod-counter");
	target.add_uniform_input("ocean-lod-data", VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
	target.add_texture_input("ocean-lods", VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);

	target.add_texture_input("ocean-height-displacement-output", VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
	target.add_texture_input("ocean-gradient-jacobian-output", VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	target.add_texture_input("ocean-normal-fft-output", VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void Ocean::setup_render_pass_resources(RenderGraph &graph)
{
	if (!vertex_mip_views.empty() && !fragment_mip_views.empty() && !normal_mip_views.empty())
		return;

	auto &vertex = graph.get_physical_texture_resource(*height_displacement_output);
	auto &fragment = graph.get_physical_texture_resource(*gradient_jacobian_output);
	auto &normal = graph.get_physical_texture_resource(*normal_fft_output);

	unsigned vertex_lods = muglm::min(unsigned(quad_lod.size()), vertex.get_image().get_create_info().levels);
	unsigned fragment_lods = fragment.get_image().get_create_info().levels;
	unsigned normal_lods = normal.get_image().get_create_info().levels;

	for (unsigned i = 0; i < vertex_lods; i++)
	{
		Vulkan::ImageViewCreateInfo view;
		view.image = &vertex.get_image();
		view.format = vertex.get_format();
		view.layers = 1;
		view.levels = 1;
		view.base_level = i;
		vertex_mip_views.push_back(graph.get_device().create_image_view(view));
	}

	for (unsigned i = 0; i < fragment_lods; i++)
	{
		Vulkan::ImageViewCreateInfo view;
		view.image = &fragment.get_image();
		view.format = fragment.get_format();
		view.layers = 1;
		view.levels = 1;
		view.base_level = i;
		fragment_mip_views.push_back(graph.get_device().create_image_view(view));
	}

	for (unsigned i = 0; i < normal_lods; i++)
	{
		Vulkan::ImageViewCreateInfo view;
		view.image = &normal.get_image();
		view.format = normal.get_format();
		view.layers = 1;
		view.levels = 1;
		view.base_level = i;
		normal_mip_views.push_back(graph.get_device().create_image_view(view));
	}
}

vec2 Ocean::get_grid_size() const
{
	return size / vec2(grid_width, grid_height);
}

vec2 Ocean::get_snapped_grid_center() const
{
	vec2 inv_grid_size = vec2(grid_width, grid_height) / size;
	vec2 grid_center = round(last_camera_position.xz() * inv_grid_size);
	return grid_center;
}

ivec2 Ocean::get_grid_base_coord() const
{
	return ivec2(get_snapped_grid_center()) - (ivec2(grid_width, grid_height) >> 1);
}

void Ocean::build_lod_map(Vulkan::CommandBuffer &cmd)
{
	auto &lod = graph->get_physical_texture_resource(*ocean_lod);
	cmd.set_storage_texture(0, 0, lod);

	vec2 grid_center = get_snapped_grid_center();
	vec2 grid_base = grid_center * get_grid_size() - 0.5f * size;

	struct Push
	{
		alignas(16) vec3 camera_pos;
		alignas(4) float max_lod;
		alignas(8) ivec2 image_offset;
		alignas(8) ivec2 num_threads;
		alignas(8) vec2 grid_base;
		alignas(8) vec2 grid_size;
	} push;
	push.camera_pos = last_camera_position;
	push.image_offset = get_grid_base_coord();
	push.num_threads = ivec2(grid_width, grid_height);
	push.grid_base = grid_base;
	push.grid_size = get_grid_size();
	push.max_lod = float(quad_lod.size()) - 1.0f;
	cmd.push_constants(&push, 0, sizeof(push));

	cmd.set_program("builtin://shaders/ocean/update_lod.comp");
	cmd.dispatch((grid_width + 7) / 8, (grid_height + 7) / 8, 1);
}

void Ocean::init_counter_buffer(Vulkan::CommandBuffer &cmd)
{
	cmd.set_storage_buffer(0, 0, graph->get_physical_buffer_resource(*lod_data_counters));
	uint32_t *vertex_counts = cmd.allocate_typed_constant_data<uint32_t>(0, 1, 16);
	for (unsigned i = 0; i < 16; i++)
		vertex_counts[i] = (i < quad_lod.size()) ? quad_lod[i].count : 0;

	cmd.set_program("builtin://shaders/ocean/init_counter_buffer.comp",
	                {{ "NUM_COUNTERS", int(MaxLODIndirect) }});
	cmd.dispatch(1, 1, 1);
}

void Ocean::cull_blocks(Vulkan::CommandBuffer &cmd)
{
	struct Push
	{
		alignas(8) ivec2 image_offset;
		alignas(8) ivec2 num_threads;
		alignas(8) vec2 inv_num_threads;
		alignas(8) vec2 grid_base;
		alignas(8) vec2 grid_size;
		alignas(8) vec2 grid_resolution;
		alignas(8) vec2 heightmap_range;
		alignas(4) uint lod_stride;
	} push;

	vec2 grid_center = get_snapped_grid_center();
	vec2 grid_base = grid_center * get_grid_size() - 0.5f * size;

	memcpy(cmd.allocate_typed_constant_data<vec4>(0, 3, 6),
	       context->get_visibility_frustum().get_planes(),
	       sizeof(vec4) * 6);

	push.image_offset = get_grid_base_coord();
	push.num_threads = ivec2(grid_width, grid_height);
	push.inv_num_threads = 1.0f / vec2(push.num_threads);
	push.grid_base = grid_base;
	push.grid_size = get_grid_size();
	push.lod_stride = grid_width * grid_height;
	push.heightmap_range = vec2(-10.0f, 10.0f);
	push.grid_resolution = vec2(grid_resolution);

	cmd.push_constants(&push, 0, sizeof(push));

	auto &lod = graph->get_physical_texture_resource(*ocean_lod);
	auto &lod_buffer = graph->get_physical_buffer_resource(*lod_data);
	cmd.set_storage_buffer(0, 0, lod_buffer);
	auto &lod_counter_buffer = graph->get_physical_buffer_resource(*lod_data_counters);
	cmd.set_storage_buffer(0, 1, lod_counter_buffer);
	cmd.set_texture(0, 2, lod, Vulkan::StockSampler::NearestWrap);

	cmd.set_program("builtin://shaders/ocean/cull_blocks.comp");
	cmd.dispatch((grid_width + 7) / 8, (grid_height + 7) / 8, 1);
}

void Ocean::update_lod_pass(Vulkan::CommandBuffer &cmd)
{
	build_lod_map(cmd);
	init_counter_buffer(cmd);

	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);

	cull_blocks(cmd);
}

void Ocean::update_fft_input(Vulkan::CommandBuffer &cmd)
{
	auto *program = cmd.get_device().get_shader_manager().register_compute("builtin://shaders/ocean/generate_fft.comp");
	unsigned height_variant = program->register_variant({});
	unsigned normal_variant = program->register_variant({{ "GRADIENT_NORMAL", 1 }});
	unsigned displacement_variant = program->register_variant({{ "GRADIENT_DISPLACEMENT", 1 }});

	struct Push
	{
		vec2 mod;
		uvec2 N;
		float time;
	};
	Push push;
	push.mod = vec2(2.0f * pi<float>()) / size;
	push.time = float(current_time);

	cmd.set_program(*program->get_program(height_variant));
	push.N = uvec2(height_fft_size, height_fft_size);
	cmd.set_storage_buffer(0, 0, *distribution_buffer);
	cmd.set_storage_buffer(0, 1, graph->get_physical_buffer_resource(*height_fft_input));
	cmd.push_constants(&push, 0, sizeof(push));
	cmd.dispatch(height_fft_size / 64, height_fft_size, 1);

	cmd.set_program(*program->get_program(displacement_variant));
	push.N = uvec2(normal_fft_size, normal_fft_size);
	cmd.set_storage_buffer(0, 0, *distribution_buffer_displacement);
	cmd.set_storage_buffer(0, 1, graph->get_physical_buffer_resource(*displacement_fft_input));
	cmd.push_constants(&push, 0, sizeof(push));
	cmd.dispatch(displacement_fft_size / 64, displacement_fft_size, 1);

	push.mod = vec2(2.0f * pi<float>()) / size_normal;
	cmd.set_program(*program->get_program(normal_variant));
	push.N = uvec2(normal_fft_size, normal_fft_size);
	cmd.set_storage_buffer(0, 0, *distribution_buffer_normal);
	cmd.set_storage_buffer(0, 1, graph->get_physical_buffer_resource(*normal_fft_input));
	cmd.push_constants(&push, 0, sizeof(push));
	cmd.dispatch(normal_fft_size / 64, normal_fft_size, 1);
}

void Ocean::compute_fft(Vulkan::CommandBuffer &cmd)
{
	FFTCommandBuffer cmd_wrapper(&cmd);

	FFTTexture height_output(&graph->get_physical_texture_resource(*height_fft_output));
	FFTBuffer height_input(&graph->get_physical_buffer_resource(*height_fft_input));
	height_fft->process(&cmd_wrapper, &height_output, &height_input);

	FFTTexture normal_output(normal_mip_views.front().get());
	FFTBuffer normal_input(&graph->get_physical_buffer_resource(*normal_fft_input));
	normal_fft->process(&cmd_wrapper, &normal_output, &normal_input);

	FFTTexture displacement_output(&graph->get_physical_texture_resource(*displacement_fft_output));
	FFTBuffer displacement_input(&graph->get_physical_buffer_resource(*displacement_fft_input));
	displacement_fft->process(&cmd_wrapper, &displacement_output, &displacement_input);
}

void Ocean::bake_maps(Vulkan::CommandBuffer &cmd)
{
	cmd.set_program("builtin://shaders/ocean/bake_maps.comp");

	struct Push
	{
		vec4 inv_size;
		vec4 scale;
	} push;

	push.inv_size = vec4(1.0f / vec2(height_fft_size), 1.0f / vec2(displacement_fft_size));
	push.scale = vec4(1.0f);
	cmd.push_constants(&push, 0, sizeof(push));

	cmd.set_texture(0, 0,
	                graph->get_physical_texture_resource(*height_fft_output),
	                Vulkan::StockSampler::LinearWrap);
	cmd.set_texture(0, 1,
	                graph->get_physical_texture_resource(*displacement_fft_output),
	                Vulkan::StockSampler::LinearWrap);
	cmd.set_storage_texture(0, 2, *vertex_mip_views.front());
	cmd.set_storage_texture(0, 3, *fragment_mip_views.front());

	cmd.dispatch((height_fft_size + 7) / 8, (height_fft_size + 7) / 8, 1);
}

void Ocean::generate_mipmaps(Vulkan::CommandBuffer &cmd)
{
	auto &normal = graph->get_physical_texture_resource(*normal_fft_output);
	auto num_passes = unsigned(muglm::max(vertex_mip_views.size(), fragment_mip_views.size()));
	num_passes = muglm::max(num_passes, normal.get_image().get_create_info().levels);

	struct Push
	{
		vec2 inv_resolution;
		uvec2 count;
		float lod;
	} push;

	for (unsigned i = 1; i < num_passes; i++)
	{
		cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

		cmd.set_program("builtin://shaders/ocean/mipmap.comp",
		                {{ "MIPMAP_RGBA16F", 1 }, { "MIPMAP_TEXEL_CENTER", 1 }});

		push.lod = float(i - 1);

		if (i < vertex_mip_views.size())
		{
			push.inv_resolution.x = 1.0f / vertex_mip_views.front()->get_image().get_width(i - 1);
			push.inv_resolution.y = 1.0f / vertex_mip_views.front()->get_image().get_height(i - 1);
			push.count.x = vertex_mip_views.front()->get_image().get_width(i);
			push.count.y = vertex_mip_views.front()->get_image().get_height(i);

			cmd.push_constants(&push, 0, sizeof(push));
			cmd.set_storage_texture(0, 0, *vertex_mip_views[i]);
			cmd.set_texture(0, 1, *vertex_mip_views[i - 1], Vulkan::StockSampler::LinearWrap);
			cmd.dispatch((push.count.x + 7) / 8, (push.count.y + 7) / 8, 1);
		}

		cmd.set_program("builtin://shaders/ocean/mipmap.comp",
		                {{ "MIPMAP_RGBA16F", 1 }});

		if (i < fragment_mip_views.size())
		{
			push.inv_resolution.x = 1.0f / fragment_mip_views.front()->get_image().get_width(i - 1);
			push.inv_resolution.y = 1.0f / fragment_mip_views.front()->get_image().get_height(i - 1);
			push.count.x = fragment_mip_views.front()->get_image().get_width(i);
			push.count.y = fragment_mip_views.front()->get_image().get_height(i);

			cmd.push_constants(&push, 0, sizeof(push));
			cmd.set_storage_texture(0, 0, *fragment_mip_views[i]);
			cmd.set_texture(0, 1, *fragment_mip_views[i - 1], Vulkan::StockSampler::LinearWrap);
			cmd.dispatch((push.count.x + 7) / 8, (push.count.y + 7) / 8, 1);
		}

		cmd.set_program("builtin://shaders/ocean/mipmap.comp",
		                {{ "MIPMAP_RG16F", 1 }});

		if (i < normal.get_image().get_create_info().levels)
		{
			push.inv_resolution.x = 1.0f / normal_mip_views.front()->get_image().get_width(i - 1);
			push.inv_resolution.y = 1.0f / normal_mip_views.front()->get_image().get_height(i - 1);
			push.count.x = normal_mip_views.front()->get_image().get_width(i);
			push.count.y = normal_mip_views.front()->get_image().get_height(i);

			cmd.push_constants(&push, 0, sizeof(push));
			cmd.set_storage_texture(0, 0, *normal_mip_views[i]);
			cmd.set_texture(0, 1, *normal_mip_views[i - 1], Vulkan::StockSampler::LinearWrap);
			cmd.dispatch((push.count.x + 7) / 8, (push.count.y + 7) / 8, 1);
		}
	}
}

void Ocean::update_fft_pass(Vulkan::CommandBuffer &cmd)
{
	update_fft_input(cmd);

	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

	compute_fft(cmd);

	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

	bake_maps(cmd);
	generate_mipmaps(cmd);
}

void Ocean::add_lod_update_pass(RenderGraph &graph)
{
	auto &update_lod = graph.add_pass("ocean-update-lods", RENDER_GRAPH_QUEUE_COMPUTE_BIT);
	AttachmentInfo lod_attachment;
	lod_attachment.format = VK_FORMAT_R16_SFLOAT;
	lod_attachment.size_x = float(grid_width);
	lod_attachment.size_y = float(grid_height);
	lod_attachment.size_class = SizeClass::Absolute;
	ocean_lod = &update_lod.add_storage_texture_output("ocean-lods", lod_attachment);

	BufferInfo lod_info_counter;
	lod_info_counter.size = MaxLODIndirect * (8 * sizeof(uint32_t));
	lod_data_counters = &update_lod.add_storage_output("ocean-lod-counter", lod_info_counter);

	BufferInfo lod_info;
	lod_info.size = grid_width * grid_height * MaxLODIndirect * (2 * sizeof(uvec4));
	lod_data = &update_lod.add_storage_output("ocean-lod-data", lod_info);

	update_lod.set_build_render_pass([&](Vulkan::CommandBuffer &cmd) {
		update_lod_pass(cmd);
	});
}

void Ocean::add_fft_update_pass(RenderGraph &graph)
{
	BufferInfo normal_info, height_info, displacement_info;
	normal_info.size = normal_fft_size * normal_fft_size * sizeof(uint32_t);
	height_info.size = height_fft_size * height_fft_size * sizeof(uint32_t);
	displacement_info.size = displacement_fft_size * displacement_fft_size * sizeof(uint32_t);

	AttachmentInfo normal_map;
	AttachmentInfo displacement_map;
	AttachmentInfo height_map;

	normal_map.size_class = SizeClass::Absolute;
	normal_map.size_x = float(normal_fft_size);
	normal_map.size_y = float(normal_fft_size);
	normal_map.format = VK_FORMAT_R16G16_SFLOAT;

	displacement_map.size_class = SizeClass::Absolute;
	displacement_map.size_x = float(displacement_fft_size);
	displacement_map.size_y = float(displacement_fft_size);
	displacement_map.format = VK_FORMAT_R16G16_SFLOAT;

	height_map.size_class = SizeClass::Absolute;
	height_map.size_x = float(height_fft_size);
	height_map.size_y = float(height_fft_size);
	height_map.format = VK_FORMAT_R16_SFLOAT;

	height_map.aux_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	displacement_map.aux_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	normal_map.aux_usage = VK_IMAGE_USAGE_SAMPLED_BIT;

	auto &update_fft = graph.add_pass("ocean-update-fft", RENDER_GRAPH_QUEUE_COMPUTE_BIT);

	height_fft_input = &update_fft.add_storage_output("ocean-height-fft-input",
	                                                  height_info);
	normal_fft_input = &update_fft.add_storage_output("ocean-normal-fft-input",
	                                                  normal_info);
	displacement_fft_input = &update_fft.add_storage_output("ocean-displacement-fft-input",
	                                                        displacement_info);

	height_fft_output = &update_fft.add_storage_texture_output("ocean-height-fft-output",
	                                                           height_map);
	normal_fft_output = &update_fft.add_storage_texture_output("ocean-normal-fft-output",
	                                                           normal_map);
	displacement_fft_output = &update_fft.add_storage_texture_output("ocean-displacement-fft-output",
	                                                                 displacement_map);

	AttachmentInfo height_displacement;
	height_displacement.size_class = SizeClass::Absolute;
	height_displacement.size_x = float(height_fft_size);
	height_displacement.size_y = float(height_fft_size);
	height_displacement.format = VK_FORMAT_R16G16B16A16_SFLOAT;

	height_displacement.levels = unsigned(quad_lod.size());

	height_displacement_output =
			&update_fft.add_storage_texture_output("ocean-height-displacement-output",
			                                       height_displacement);

	height_displacement.levels = 0;

	gradient_jacobian_output =
			&update_fft.add_storage_texture_output("ocean-gradient-jacobian-output",
			                                       height_displacement);

	update_fft.set_build_render_pass([&](Vulkan::CommandBuffer &cmd) {
		update_fft_pass(cmd);
	});
}

void Ocean::add_render_passes(RenderGraph &graph)
{
	this->graph = &graph;
	add_lod_update_pass(graph);
	add_fft_update_pass(graph);
}

struct OceanData
{
	vec2 inv_heightmap_size;
	vec2 normal_uv_scale;
	vec2 integer_to_world_mod;
	vec2 heightmap_range;
};

struct OceanInfo
{
	Vulkan::Program *program;
	const Vulkan::Buffer *ubo;
	const Vulkan::Buffer *indirect;
	const Vulkan::Buffer *vbos[MaxLODIndirect];
	const Vulkan::Buffer *ibos[MaxLODIndirect];

	const Vulkan::ImageView *heightmap;
	const Vulkan::ImageView *lod_map;
	const Vulkan::ImageView *grad_jacobian;
	const Vulkan::ImageView *normal;

	unsigned lods;
	unsigned lod_stride;
	OceanData data;
};

namespace RenderFunctions
{
static void ocean_render(Vulkan::CommandBuffer &cmd, const RenderQueueData *infos, unsigned num_instances)
{
	auto &ocean_info = *static_cast<const OceanInfo *>(infos->render_info);

	cmd.set_program(*ocean_info.program);
	cmd.set_vertex_attrib(0, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(OceanVertex, pos));
	cmd.set_vertex_attrib(1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(OceanVertex, weights));
	cmd.set_primitive_restart(true);
	cmd.set_wireframe(true);
	cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);

	for (unsigned instance = 0; instance < num_instances; instance++)
	{
		cmd.push_constants(&ocean_info.data, 0, sizeof(ocean_info.data));
		cmd.set_texture(2, 0, *ocean_info.heightmap, Vulkan::StockSampler::LinearWrap);
		cmd.set_texture(2, 1, *ocean_info.lod_map, Vulkan::StockSampler::LinearWrap);
		cmd.set_texture(2, 2, *ocean_info.grad_jacobian, Vulkan::StockSampler::TrilinearWrap);
		cmd.set_texture(2, 3, *ocean_info.normal, Vulkan::StockSampler::TrilinearWrap);

		for (unsigned lod = 0; lod < ocean_info.lods; lod++)
		{
			cmd.set_uniform_buffer(3, 0, *ocean_info.ubo,
			                       ocean_info.lod_stride * lod,
			                       ocean_info.lod_stride);

			cmd.set_vertex_binding(0, *ocean_info.vbos[lod], 0, 8);
			cmd.set_index_buffer(*ocean_info.ibos[lod], 0, VK_INDEX_TYPE_UINT16);
			cmd.draw_indexed_indirect(*ocean_info.indirect, 8 * sizeof(uint32_t) * lod, 1, 8 * sizeof(uint32_t));
		}
	}
}
}

void Ocean::get_render_info(const RenderContext &,
                            const CachedSpatialTransformComponent *,
                            RenderQueue &queue) const
{
	Util::Hasher hasher;

	auto &ubo = graph->get_physical_buffer_resource(*lod_data);
	auto &indirect = graph->get_physical_buffer_resource(*lod_data_counters);
	auto &lod = graph->get_physical_texture_resource(*ocean_lod);
	auto &normal = graph->get_physical_texture_resource(*normal_fft_output);
	auto &height_displacement = graph->get_physical_texture_resource(*height_displacement_output);
	auto &grad_jacobian = graph->get_physical_texture_resource(*gradient_jacobian_output);

	hasher.string("ocean");
	hasher.u64(lod.get_cookie());
	hasher.u64(normal.get_cookie());
	hasher.u64(height_displacement.get_cookie());
	hasher.u64(grad_jacobian.get_cookie());
	hasher.u64(ubo.get_cookie());
	hasher.u64(indirect.get_cookie());
	auto instance_key = hasher.get();

	auto *patch_data = queue.push<OceanInfo>(Queue::Opaque, instance_key, 1,
	                                         RenderFunctions::ocean_render,
	                                         nullptr);

	if (patch_data)
	{
		patch_data->program =
				queue.get_shader_suites()[Util::ecast(RenderableType::Ocean)].get_program(DrawPipeline::Opaque,
				                                                                          MESH_ATTRIBUTE_POSITION_BIT,
				                                                                          MATERIAL_TEXTURE_BASE_COLOR_BIT);

		patch_data->heightmap = &height_displacement;
		patch_data->lod_map = &lod;
		patch_data->grad_jacobian = &grad_jacobian;
		patch_data->normal = &normal;

		patch_data->ubo = &ubo;
		patch_data->indirect = &indirect;
		patch_data->lod_stride = grid_width * grid_height * 2 * sizeof(vec4);
		patch_data->lods = unsigned(quad_lod.size());
		patch_data->data.inv_heightmap_size = 1.0f / vec2(height_fft_size);
		patch_data->data.integer_to_world_mod = get_grid_size() / vec2(grid_resolution);
		patch_data->data.normal_uv_scale = size / size_normal;
		patch_data->data.heightmap_range = vec2(-10.0f, 10.0f);

		for (unsigned i = 0; i < unsigned(quad_lod.size()); i++)
		{
			patch_data->vbos[i] = quad_lod[i].vbo.get();
			patch_data->ibos[i] = quad_lod[i].ibo.get();
		}
	}
}

void Ocean::build_lod(Vulkan::Device &device, unsigned size, unsigned stride)
{
	unsigned size_1 = size + 1;
	vector<OceanVertex> vertices;
	vertices.reserve(size_1 * size_1);
	vector<uint16_t> indices;
	indices.reserve(size * (2 * size_1 + 1));

	unsigned half_size = grid_resolution >> 1;

	for (unsigned y = 0; y <= grid_resolution; y += stride)
	{
		for (unsigned x = 0; x <= grid_resolution; x += stride)
		{
			OceanVertex v = {};
			v.pos[0] = uint8_t(x);
			v.pos[1] = uint8_t(y);
			v.pos[2] = uint8_t(x < half_size);
			v.pos[3] = uint8_t(y < half_size);

			if (x == 0)
				v.weights[0] = 255;
			else if (x == grid_resolution)
				v.weights[1] = 255;
			else if (y == 0)
				v.weights[2] = 255;
			else if (y == grid_resolution)
				v.weights[3] = 255;

			vertices.push_back(v);
		}
	}

	unsigned slices = size;
	for (unsigned slice = 0; slice < slices; slice++)
	{
		unsigned base = slice * size_1;
		for (unsigned x = 0; x <= size; x++)
		{
			indices.push_back(base + x);
			indices.push_back(base + size_1 + x);
		}
		indices.push_back(0xffffu);
	}

	Vulkan::BufferCreateInfo info = {};
	info.size = vertices.size() * sizeof(OceanVertex);
	info.domain = Vulkan::BufferDomain::Device;
	info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

	LOD lod;
	lod.vbo = device.create_buffer(info, vertices.data());

	info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	info.size = indices.size() * sizeof(uint16_t);
	lod.ibo = device.create_buffer(info, indices.data());
	lod.count = indices.size();

	quad_lod.push_back(lod);
}

void Ocean::build_buffers(Vulkan::Device &device)
{
	unsigned size = grid_resolution;
	unsigned stride = 1;
	while (size >= 2)
	{
		build_lod(device, size, stride);
		size >>= 1;
		stride <<= 1;
	}
}

void Ocean::init_distributions(Vulkan::Device &device)
{
	Vulkan::BufferCreateInfo height_distribution = {};
	height_distribution.domain = Vulkan::BufferDomain::Device;
	height_distribution.misc = Vulkan::BUFFER_MISC_ZERO_INITIALIZE_BIT;
	height_distribution.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

	auto displacement_distribution = height_distribution;
	auto normal_distribution = height_distribution;

	height_distribution.size = height_fft_size * height_fft_size * sizeof(vec2);
	displacement_distribution.size = displacement_fft_size * displacement_fft_size * sizeof(vec2);
	normal_distribution.size = normal_fft_size * normal_fft_size * sizeof(vec2);

	distribution_buffer = device.create_buffer(height_distribution, nullptr);
	distribution_buffer_displacement = device.create_buffer(displacement_distribution, nullptr);
	distribution_buffer_normal = device.create_buffer(normal_distribution, nullptr);
}

}