
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2013 Francois Beaune, Jupiter Jazz Limited
// Copyright (c) 2014 Francois Beaune, The appleseedhq Organization
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "project.h"

// appleseed.renderer headers.
#include "renderer/global/globallogger.h"
#include "renderer/kernel/aov/imagestack.h"
#include "renderer/kernel/aov/spectrumstack.h"
#include "renderer/kernel/intersection/tracecontext.h"
#include "renderer/modeling/edf/edf.h"
#include "renderer/modeling/environment/environment.h"
#include "renderer/modeling/environmentedf/environmentedf.h"
#include "renderer/modeling/environmentshader/environmentshader.h"
#include "renderer/modeling/frame/frame.h"
#include "renderer/modeling/light/light.h"
#include "renderer/modeling/material/material.h"
#include "renderer/modeling/object/object.h"
#include "renderer/modeling/project/configuration.h"
#include "renderer/modeling/project/configurationcontainer.h"
#include "renderer/modeling/scene/assembly.h"
#include "renderer/modeling/scene/assemblyinstance.h"
#include "renderer/modeling/scene/basegroup.h"
#include "renderer/modeling/scene/containers.h"
#include "renderer/modeling/scene/objectinstance.h"
#include "renderer/modeling/scene/scene.h"
#include "renderer/modeling/surfaceshader/surfaceshader.h"

// appleseed.foundation headers.
#include "foundation/image/canvasproperties.h"
#include "foundation/image/image.h"
#include "foundation/image/pixel.h"
#include "foundation/platform/types.h"
#include "foundation/utility/foreach.h"
#include "foundation/utility/searchpaths.h"

// Standard headers.
#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace foundation;
using namespace std;

namespace renderer
{

//
// Project class implementation.
//

namespace
{
    const UniqueID g_class_uid = new_guid();
}

UniqueID Project::get_class_uid()
{
    return g_class_uid;
}

namespace
{
    // Revision number of the project format.
    const size_t ProjectFormatRevision = 7;
}

struct Project::Impl
{
    size_t                      m_format_revision;
    string                      m_path;
    auto_release_ptr<Scene>     m_scene;
    auto_release_ptr<Frame>     m_frame;
    RenderLayerRuleContainer    m_render_layer_rules;
    ConfigurationContainer      m_configurations;
    SearchPaths                 m_search_paths;
    auto_ptr<TraceContext>      m_trace_context;

    Impl()
      : m_format_revision(ProjectFormatRevision)
    {
    }
};

Project::Project(const char* name)
  : Entity(g_class_uid)
  , impl(new Impl())
{
    set_name(name);
    add_base_configurations();
}

Project::~Project()
{
    delete impl;
}

void Project::release()
{
    delete this;
}

void Project::set_format_revision(const size_t format_revision)
{
    impl->m_format_revision = format_revision;
}

size_t Project::get_format_revision() const
{
    return impl->m_format_revision;
}

bool Project::has_path() const
{
    return impl->m_path.size() > 0;
}

void Project::set_path(const char* path)
{
    assert(path);
    impl->m_path = path;
}

const char* Project::get_path() const
{
    return impl->m_path.c_str();
}

SearchPaths& Project::search_paths() const
{
    return impl->m_search_paths;
}

void Project::set_scene(auto_release_ptr<Scene> scene)
{
    impl->m_scene = scene;
}

Scene* Project::get_scene() const
{
    return impl->m_scene.get();
}

void Project::set_frame(auto_release_ptr<Frame> frame)
{
    impl->m_frame = frame;
}

Frame* Project::get_frame() const
{
    return impl->m_frame.get();
}

void Project::add_render_layer_rule(foundation::auto_release_ptr<RenderLayerRule> rule)
{
    impl->m_render_layer_rules.insert(rule);
}

RenderLayerRuleContainer& Project::render_layer_rules() const
{
    return impl->m_render_layer_rules;
}

ConfigurationContainer& Project::configurations() const
{
    return impl->m_configurations;
}

void Project::add_default_configurations()
{
    add_default_configuration("final", "base_final");
    add_default_configuration("interactive", "base_interactive");
}

namespace
{
    struct RuleOrderingPredicate
    {
        bool operator()(const RenderLayerRule* lhs, const RenderLayerRule* rhs) const
        {
            return lhs->get_order() < rhs->get_order();
        }
    };

    typedef vector<const RenderLayerRule*> RenderLayerRuleVector;
    typedef map<string, size_t> RenderLayerMapping;

    void apply_render_layer_rule_to_entity(
        const RenderLayerRule&          rule,
        RenderLayerMapping&             mapping,
        ImageStack&                     images,
        const PixelFormat               format,
        Entity&                         entity)
    {
        const string render_layer_name = rule.get_render_layer();

        if (render_layer_name.empty())
        {
            entity.set_render_layer_index(~0);
            return;
        }

        const RenderLayerMapping::const_iterator i = mapping.find(render_layer_name);

        if (i != mapping.end())
        {
            entity.set_render_layer_index(i->second);
            return;
        }

        assert(mapping.size() <= SpectrumStack::MaxSize);

        if (mapping.size() == SpectrumStack::MaxSize)
        {
            RENDERER_LOG_ERROR(
                "while assigning entity \"%s\" to render layer \"%s\": "
                "could not create render layer, maximum number of AOVs (" FMT_SIZE_T ") reached.",
                entity.get_name(),
                render_layer_name.c_str(),
                SpectrumStack::MaxSize);
            entity.set_render_layer_index(~0);
            return;
        }

        const size_t image_index = images.append(render_layer_name.c_str(), format);

        mapping[render_layer_name] = image_index;
        entity.set_render_layer_index(image_index);
    }

    void apply_render_layer_rules_to_entity(
        const RenderLayerRuleVector&    rules,
        RenderLayerMapping&             mapping,
        ImageStack&                     images,
        const PixelFormat               format,
        Entity&                         entity)
    {
        for (const_each<RenderLayerRuleVector> r = rules; r; ++r)
        {
            const RenderLayerRule* rule = *r;

            if (rule->get_entity_type_uid() == ~0 ||
                rule->get_entity_type_uid() == entity.get_class_uid())
            {
                if (rule->applies(entity))
                {
                    RENDERER_LOG_DEBUG(
                        "assigning entity \"%s\" to render layer \"%s\" (via rule \"%s\").",
                        entity.get_name(),
                        rule->get_render_layer(),
                        rule->get_name());

                    apply_render_layer_rule_to_entity(*rule, mapping, images, format, entity);

                    break;
                }
            }
        }
    }

    template <typename EntityCollection>
    void apply_render_layer_rules_to_entities(
        const RenderLayerRuleVector&    rules,
        RenderLayerMapping&             mapping,
        ImageStack&                     images,
        const PixelFormat               format,
        EntityCollection&               entities)
    {
        for (each<EntityCollection> i = entities; i; ++i)
            apply_render_layer_rules_to_entity(rules, mapping, images, format, *i);
    }

    void apply_render_layer_rules_to_base_group(
        const RenderLayerRuleVector&    rules,
        RenderLayerMapping&             mapping,
        ImageStack&                     images,
        const PixelFormat               format,
        BaseGroup&                      base_group);

    void apply_render_layer_rules_to_assembly(
        const RenderLayerRuleVector&    rules,
        RenderLayerMapping&             mapping,
        ImageStack&                     images,
        const PixelFormat               format,
        Assembly&                       assembly)
    {
        apply_render_layer_rules_to_base_group(rules, mapping, images, format, assembly);
        apply_render_layer_rules_to_entities(rules, mapping, images, format, assembly.edfs());
        apply_render_layer_rules_to_entities(rules, mapping, images, format, assembly.lights());
        apply_render_layer_rules_to_entities(rules, mapping, images, format, assembly.materials());
        apply_render_layer_rules_to_entities(rules, mapping, images, format, assembly.objects());
        apply_render_layer_rules_to_entities(rules, mapping, images, format, assembly.object_instances());
        apply_render_layer_rules_to_entities(rules, mapping, images, format, assembly.surface_shaders());
    }

    void apply_render_layer_rules_to_base_group(
        const RenderLayerRuleVector&    rules,
        RenderLayerMapping&             mapping,
        ImageStack&                     images,
        const PixelFormat               format,
        BaseGroup&                      base_group)
    {
        apply_render_layer_rules_to_entities(rules, mapping, images, format, base_group.assemblies());
        apply_render_layer_rules_to_entities(rules, mapping, images, format, base_group.assembly_instances());

        for (each<AssemblyContainer> i = base_group.assemblies(); i; ++i)
            apply_render_layer_rules_to_assembly(rules, mapping, images, format, *i);
    }

    void apply_render_layer_rules_to_scene(
        const RenderLayerRuleVector&    rules,
        ImageStack&                     images,
        const PixelFormat               format,
        Scene&                          scene)
    {
        RenderLayerMapping mapping;

        apply_render_layer_rules_to_base_group(rules, mapping, images, format, scene);

        EnvironmentEDF* env_edf = scene.get_environment()->get_uncached_environment_edf();

        if (env_edf)
            apply_render_layer_rules_to_entity(rules, mapping, images, format, *env_edf);

        EnvironmentShader* env_shader = scene.get_environment()->get_uncached_environment_shader();

        if (env_shader)
            apply_render_layer_rules_to_entity(rules, mapping, images, format, *env_shader);
    }
}

void Project::create_aov_images()
{
    RenderLayerRuleVector rules;

    for (const_each<RenderLayerRuleContainer> i = impl->m_render_layer_rules; i; ++i)
        rules.push_back(&*i);

    sort(rules.begin(), rules.end(), RuleOrderingPredicate());

    assert(impl->m_frame.get());

    ImageStack& aov_images = impl->m_frame->aov_images();
    const PixelFormat aov_format = impl->m_frame->image().properties().m_pixel_format;

    aov_images.clear();

    apply_render_layer_rules_to_scene(
        rules,
        aov_images,
        aov_format,
        impl->m_scene.ref());
}

bool Project::has_trace_context() const
{
    return impl->m_trace_context.get() != 0;
}

const TraceContext& Project::get_trace_context() const
{
    if (impl->m_trace_context.get() == 0)
    {
        assert(impl->m_scene.get());
        impl->m_trace_context.reset(new TraceContext(*impl->m_scene));
    }

    return *impl->m_trace_context;
}

void Project::update_trace_context()
{
    if (impl->m_trace_context.get())
        impl->m_trace_context->update();
}

void Project::add_base_configurations()
{
    impl->m_configurations.insert(BaseConfigurationFactory::create_base_final());
    impl->m_configurations.insert(BaseConfigurationFactory::create_base_interactive());
}

void Project::add_default_configuration(const char* name, const char* base_name)
{
    Configuration* base_configuration = impl->m_configurations.get_by_name(base_name);
    assert(base_configuration);

    auto_release_ptr<Configuration> configuration = ConfigurationFactory::create(name);
    configuration->set_base(base_configuration);

    impl->m_configurations.insert(configuration);
}


//
// ProjectFactory class implementation.
//

auto_release_ptr<Project> ProjectFactory::create(const char* name)
{
    return auto_release_ptr<Project>(new Project(name));
}

}   // namespace renderer
