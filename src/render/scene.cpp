#include <mitsuba/core/properties.h>
#include <mitsuba/core/plugin.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/medium.h>
#include <mitsuba/render/scene.h>
#include <mitsuba/render/integrator.h>

#if defined(MI_ENABLE_EMBREE)
#  include "scene_embree.inl"
#else
#  include <mitsuba/render/kdtree.h>
#  include "scene_native.inl"
#endif

#if defined(MI_ENABLE_CUDA)
#  include "scene_optix.inl"
#endif

NAMESPACE_BEGIN(mitsuba)

MI_VARIANT Scene<Float, Spectrum>::Scene(const Properties &props) {
    for (auto &kv : props.objects()) {
        m_children.push_back(kv.second.get());

        Shape *shape           = dynamic_cast<Shape *>(kv.second.get());
        Emitter *emitter       = dynamic_cast<Emitter *>(kv.second.get());
        Sensor *sensor         = dynamic_cast<Sensor *>(kv.second.get());
        Integrator *integrator = dynamic_cast<Integrator *>(kv.second.get());

        if (shape) {
            if (shape->is_emitter())
                m_emitters.push_back(shape->emitter());
            if (shape->is_sensor())
                m_sensors.push_back(shape->sensor());
            if (shape->is_shapegroup()) {
                m_shapegroups.push_back((ShapeGroup*)shape);
            } else {
                m_bbox.expand(shape->bbox());
                m_shapes.push_back(shape);
            }
        } else if (emitter) {
            // Surface emitters will be added to the list when attached to a shape
            if (!has_flag(emitter->flags(), EmitterFlags::Surface))
                m_emitters.push_back(emitter);

            if (emitter->is_environment()) {
                if (m_environment)
                    Throw("Only one environment emitter can be specified per scene.");
                m_environment = emitter;
            }
        } else if (sensor) {
            m_sensors.push_back(sensor);
        } else if (integrator) {
            if (m_integrator)
                Throw("Only one integrator can be specified per scene.");
            m_integrator = integrator;
        }
    }

    if (m_sensors.empty()) {
        Log(Warn, "No sensors found! Instantiating a perspective camera..");
        Properties sensor_props("perspective");
        sensor_props.set_float("fov", 45.0);

        /* Create a perspective camera with a 45 deg. field of view
           and positioned so that it can see the entire scene */
        if (m_bbox.valid()) {
            ScalarPoint3f center = m_bbox.center();
            ScalarVector3f extents = m_bbox.extents();

            ScalarFloat distance =
                dr::hmax(extents) / (2.f * dr::tan(45.f * .5f * dr::Pi<ScalarFloat> / 180.f));

            sensor_props.set_float("far_clip", (Properties::Float) (dr::hmax(extents) * 5 + distance));
            sensor_props.set_float("near_clip", (Properties::Float) distance / 100);

            sensor_props.set_float("focus_distance", (Properties::Float) (distance + extents.z() / 2));
            sensor_props.set_transform(
                "to_world", ScalarTransform4f::translate(ScalarVector3f(
                                center.x(), center.y(), m_bbox.min.z() - distance)));
        }

        m_sensors.push_back(
            PluginManager::instance()->create_object<Sensor>(sensor_props));
    }

    // Create sensors' shapes (environment sensors)
    for (Sensor *sensor: m_sensors)
        sensor->set_scene(this);

    if (!m_integrator) {
        Log(Warn, "No integrator found! Instantiating a path tracer..");
        m_integrator = PluginManager::instance()->
            create_object<Integrator>(Properties("path"));
    }

    if constexpr (dr::is_cuda_array_v<Float>)
        accel_init_gpu(props);
    else
        accel_init_cpu(props);

    if (!m_emitters.empty()) {
        // Inform environment emitters etc. about the scene bounds
        for (Emitter *emitter: m_emitters)
            emitter->set_scene(this);

    }

    m_shapes_dr = dr::load<DynamicBuffer<ShapePtr>>(
        m_shapes.data(), m_shapes.size());

    m_emitters_dr = dr::load<DynamicBuffer<EmitterPtr>>(
        m_emitters.data(), m_emitters.size());

    m_emitter_pmf = m_emitters.empty() ? 0.f : (1.f / m_emitters.size());

    m_shapes_grad_enabled = false;
}

MI_VARIANT Scene<Float, Spectrum>::~Scene() {
    if constexpr (dr::is_cuda_array_v<Float>)
        accel_release_gpu();
    else
        accel_release_cpu();

    // Trigger deallocation of all instances
    m_emitters.clear();
    m_shapes.clear();
    m_shapegroups.clear();
    m_sensors.clear();
    m_children.clear();
    m_integrator = nullptr;
    m_environment = nullptr;

    if constexpr (dr::is_jit_array_v<Float>) {
        // Clean up JIT pointer registry now that the above has happened
        jit_registry_trim();
    }
}

MI_VARIANT ref<Bitmap>
Scene<Float, Spectrum>::render(uint32_t sensor_index, uint32_t seed, uint32_t spp) {
    m_integrator->render(this, sensor_index, seed, spp, /* develop = */ false);
    return m_sensors[sensor_index]->film()->bitmap();
}

// -----------------------------------------------------------------------

MI_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect(const Ray3f &ray, uint32_t ray_flags, Mask coherent, Mask active) const {
    MI_MASKED_FUNCTION(ProfilerPhase::RayIntersect, active);
    DRJIT_MARK_USED(coherent);

    if constexpr (dr::is_cuda_array_v<Float>)
        return ray_intersect_gpu(ray, ray_flags, active);
    else
        return ray_intersect_cpu(ray, ray_flags, coherent, active);
}

MI_VARIANT typename Scene<Float, Spectrum>::PreliminaryIntersection3f
Scene<Float, Spectrum>::ray_intersect_preliminary(const Ray3f &ray, Mask coherent, Mask active) const {
    DRJIT_MARK_USED(coherent);
    if constexpr (dr::is_cuda_array_v<Float>)
        return ray_intersect_preliminary_gpu(ray, active);
    else
        return ray_intersect_preliminary_cpu(ray, coherent, active);
}

MI_VARIANT typename Scene<Float, Spectrum>::Mask
Scene<Float, Spectrum>::ray_test(const Ray3f &ray, Mask coherent, Mask active) const {
    MI_MASKED_FUNCTION(ProfilerPhase::RayTest, active);
    DRJIT_MARK_USED(coherent);

    if constexpr (dr::is_cuda_array_v<Float>)
        return ray_test_gpu(ray, active);
    else
        return ray_test_cpu(ray, coherent, active);
}

MI_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect_naive(const Ray3f &ray, Mask active) const {
    MI_MASKED_FUNCTION(ProfilerPhase::RayIntersect, active);

#if !defined(MI_ENABLE_EMBREE)
    if constexpr (!dr::is_cuda_array_v<Float>)
        return ray_intersect_naive_cpu(ray, active);
#endif
    DRJIT_MARK_USED(ray);
    DRJIT_MARK_USED(active);
    NotImplementedError("ray_intersect_naive");
}

// -----------------------------------------------------------------------

MI_VARIANT std::tuple<typename Scene<Float, Spectrum>::UInt32, Float, Float>
Scene<Float, Spectrum>::sample_emitter(Float index_sample, Mask active) const {
    MI_MASKED_FUNCTION(ProfilerPhase::SampleEmitter, active);

    if (unlikely(m_emitters.size() < 2)) {
        if (m_emitters.size() == 1)
            return { UInt32(0), 1.f, index_sample };
        else
            return { UInt32(-1), 0.f, index_sample };
    }

    uint32_t emitter_count = (uint32_t) m_emitters.size();
    ScalarFloat emitter_count_f = (ScalarFloat) emitter_count;
    Float index_sample_scaled = index_sample * emitter_count_f;

    UInt32 index = dr::min(UInt32(index_sample_scaled), emitter_count - 1u);

    return { index, emitter_count_f, index_sample_scaled - Float(index) };
}

MI_VARIANT Float Scene<Float, Spectrum>::pdf_emitter(UInt32 /*index*/,
                                                      Mask /*active*/) const {
    return m_emitter_pmf;
}

MI_VARIANT std::tuple<typename Scene<Float, Spectrum>::Ray3f, Spectrum,
                       const typename Scene<Float, Spectrum>::EmitterPtr>
Scene<Float, Spectrum>::sample_emitter_ray(Float time, Float sample1,
                                           const Point2f &sample2,
                                           const Point2f &sample3,
                                           Mask active) const {
    MI_MASKED_FUNCTION(ProfilerPhase::SampleEmitterRay, active);


    Ray3f ray;
    Spectrum weight;
    EmitterPtr emitter;

    // Potentially disable inlining of emitter sampling (if there is just a single emitter)
    bool vcall_inline = true;
    if constexpr (dr::is_jit_array_v<Float>)
         vcall_inline = jit_flag(JitFlag::VCallInline);

    size_t emitter_count = m_emitters.size();
    if (emitter_count > 1 || (emitter_count == 1 && !vcall_inline)) {
        auto [index, emitter_weight, sample_1_re] = sample_emitter(sample1, active);
        EmitterPtr emitter = dr::gather<EmitterPtr>(m_emitters_dr, index, active);

        std::tie(ray, weight) =
            emitter->sample_ray(time, sample_1_re, sample2, sample3, active);

        weight *= emitter_weight;
    } else if (emitter_count == 1) {
        std::tie(ray, weight) =
            m_emitters[0]->sample_ray(time, sample1, sample2, sample3, active);
    } else {
        ray = dr::zero<Ray3f>();
        weight = dr::zero<Spectrum>();
        emitter = EmitterPtr(nullptr);
    }

    return { ray, weight, emitter };
}

MI_VARIANT std::pair<typename Scene<Float, Spectrum>::DirectionSample3f, Spectrum>
Scene<Float, Spectrum>::sample_emitter_direction(const Interaction3f &ref, const Point2f &sample_,
                                                 bool test_visibility, Mask active) const {
    MI_MASKED_FUNCTION(ProfilerPhase::SampleEmitterDirection, active);

    Point2f sample(sample_);
    DirectionSample3f ds;
    Spectrum spec;

    // Potentially disable inlining of emitter sampling (if there is just a single emitter)
    bool vcall_inline = true;
    if constexpr (dr::is_jit_array_v<Float>)
         vcall_inline = jit_flag(JitFlag::VCallInline);

    size_t emitter_count = m_emitters.size();
    if (emitter_count > 1 || (emitter_count == 1 && !vcall_inline)) {
        // Randomly pick an emitter
        auto [index, emitter_weight, sample_x_re] = sample_emitter(sample.x(), active);
        sample.x() = sample_x_re;

        // Sample a direction towards the emitter
        EmitterPtr emitter = dr::gather<EmitterPtr>(m_emitters_dr, index, active);
        std::tie(ds, spec) = emitter->sample_direction(ref, sample, active);

        // Account for the discrete probability of sampling this emitter
        ds.pdf *= pdf_emitter(index, active);
        spec *= emitter_weight;

        active &= dr::neq(ds.pdf, 0.f);

        // Mark occluded samles as invalid if requested by the user
        if (test_visibility && dr::any_or<true>(active)) {
            Mask occluded = ray_test(ref.spawn_ray_to(ds.p), active);
            dr::masked(spec, occluded) = 0.f;
            dr::masked(ds.pdf, occluded) = 0.f;
        }
    } else if (emitter_count == 1) {
        // Sample a direction towards the (single) emitter
        std::tie(ds, spec) = m_emitters[0]->sample_direction(ref, sample, active);

        active &= dr::neq(ds.pdf, 0.f);

        // Mark occluded samles as invalid if requested by the user
        if (test_visibility && dr::any_or<true>(active)) {
            Mask occluded = ray_test(ref.spawn_ray_to(ds.p), active);
            dr::masked(spec, occluded) = 0.f;
            dr::masked(ds.pdf, occluded) = 0.f;
        }
    } else {
        ds = dr::zero<DirectionSample3f>();
        spec = 0.f;
    }

    return { ds, spec };
}

MI_VARIANT Float
Scene<Float, Spectrum>::pdf_emitter_direction(const Interaction3f &ref,
                                              const DirectionSample3f &ds,
                                              Mask active) const {
    MI_MASK_ARGUMENT(active);
    return ds.emitter->pdf_direction(ref, ds, active) * m_emitter_pmf;
}

MI_VARIANT Spectrum Scene<Float, Spectrum>::eval_emitter_direction(
    const Interaction3f &ref, const DirectionSample3f &ds, Mask active) const {
    MI_MASK_ARGUMENT(active);
    return ds.emitter->eval_direction(ref, ds, active);
}

MI_VARIANT void Scene<Float, Spectrum>::traverse(TraversalCallback *callback) {
    for (auto& child : m_children) {
        std::string id = child->id();
        if (id.empty() || string::starts_with(id, "_unnamed_"))
            id = child->class_()->name();
        callback->put_object(id, child.get());
    }
}

MI_VARIANT void Scene<Float, Spectrum>::parameters_changed(const std::vector<std::string> &/*keys*/) {
    if (m_environment)
        m_environment->set_scene(this); // TODO use parameters_changed({"scene"})

    bool accel_is_dirty = false;
    for (auto &s : m_shapes) {
        accel_is_dirty |= s->dirty();
        s->m_dirty = false;
    }

    if (accel_is_dirty) {
        if constexpr (dr::is_cuda_array_v<Float>)
            accel_parameters_changed_gpu();
        else
            accel_parameters_changed_cpu();
    }

    // Check whether any shape parameters have gradient tracking enabled
    m_shapes_grad_enabled = false;
    for (auto &s : m_shapes) {
        m_shapes_grad_enabled |= s->parameters_grad_enabled();
        if (m_shapes_grad_enabled)
            break;
    }
}

MI_VARIANT std::string Scene<Float, Spectrum>::to_string() const {
    std::ostringstream oss;
    oss << "Scene[" << std::endl
        << "  children = [" << std::endl;
    for (size_t i = 0; i < m_children.size(); ++i) {
        oss << "    " << string::indent(m_children[i], 4);
        if (i + 1 < m_children.size())
            oss << ",";
        oss <<  std::endl;
    }
    oss << "  ]"<< std::endl
        << "]";
    return oss.str();
}

MI_VARIANT void Scene<Float, Spectrum>::static_accel_initialization() {
    if constexpr (dr::is_cuda_array_v<Float>)
        Scene::static_accel_initialization_gpu();
    else
        Scene::static_accel_initialization_cpu();
}

MI_VARIANT void Scene<Float, Spectrum>::static_accel_shutdown() {
    if constexpr (dr::is_cuda_array_v<Float>)
        Scene::static_accel_shutdown_gpu();
    else
        Scene::static_accel_shutdown_cpu();
}

MI_VARIANT void Scene<Float, Spectrum>::static_accel_initialization_cpu() { }
MI_VARIANT void Scene<Float, Spectrum>::static_accel_shutdown_cpu() { }

void librender_nop() { }

#if !defined(MI_ENABLE_CUDA)
MI_VARIANT void Scene<Float, Spectrum>::accel_init_gpu(const Properties &) {
    NotImplementedError("accel_init_gpu");
}
MI_VARIANT void Scene<Float, Spectrum>::accel_parameters_changed_gpu() {
    NotImplementedError("accel_parameters_changed_gpu");
}
MI_VARIANT void Scene<Float, Spectrum>::accel_release_gpu() {
    NotImplementedError("accel_release_gpu");
}
MI_VARIANT typename Scene<Float, Spectrum>::PreliminaryIntersection3f
Scene<Float, Spectrum>::ray_intersect_preliminary_gpu(const Ray3f &, Mask) const {
    NotImplementedError("ray_intersect_preliminary_gpu");
}
MI_VARIANT typename Scene<Float, Spectrum>::SurfaceInteraction3f
Scene<Float, Spectrum>::ray_intersect_gpu(const Ray3f &, uint32_t, Mask) const {
    NotImplementedError("ray_intersect_naive_gpu");
}
MI_VARIANT typename Scene<Float, Spectrum>::Mask
Scene<Float, Spectrum>::ray_test_gpu(const Ray3f &, Mask) const {
    NotImplementedError("ray_test_gpu");
}
MI_VARIANT void Scene<Float, Spectrum>::static_accel_initialization_gpu() { }
MI_VARIANT void Scene<Float, Spectrum>::static_accel_shutdown_gpu() { }
#endif

MI_IMPLEMENT_CLASS_VARIANT(Scene, Object, "scene")
MI_INSTANTIATE_CLASS(Scene)
NAMESPACE_END(mitsuba)