
//
// This source file is part of appleseed.
// Visit https://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2014-2018 Esteban Tovagliari, The appleseedhq Organization
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
#include "closures.h"

// appleseed.renderer headers.
#include "renderer/global/globallogger.h"
#include "renderer/kernel/shading/oslshadingsystem.h"
#include "renderer/modeling/bsdf/ashikhminbrdf.h"
#include "renderer/modeling/bsdf/blinnbrdf.h"
#include "renderer/modeling/bsdf/diffusebtdf.h"
#include "renderer/modeling/bsdf/disneybrdf.h"
#include "renderer/modeling/bsdf/glassbsdf.h"
#include "renderer/modeling/bsdf/glossybrdf.h"
#include "renderer/modeling/bsdf/metalbrdf.h"
#include "renderer/modeling/bsdf/microfacethelper.h"
#include "renderer/modeling/bsdf/orennayarbrdf.h"
#include "renderer/modeling/bsdf/plasticbrdf.h"
#include "renderer/modeling/bsdf/sheenbrdf.h"
#include "renderer/modeling/bssrdf/dipolebssrdf.h"
#include "renderer/modeling/bssrdf/directionaldipolebssrdf.h"
#include "renderer/modeling/bssrdf/gaussianbssrdf.h"
#include "renderer/modeling/bssrdf/normalizeddiffusionbssrdf.h"
#include "renderer/modeling/bssrdf/randomwalkbssrdf.h"
#include "renderer/modeling/color/colorspace.h"
#include "renderer/modeling/edf/diffuseedf.h"

// appleseed.foundation headers.
#include "foundation/math/cdf.h"
#include "foundation/math/fresnel.h"
#include "foundation/math/scalar.h"
#include "foundation/utility/arena.h"
#include "foundation/utility/memory.h"
#include "foundation/utility/otherwise.h"

// OSL headers.
#include "foundation/platform/_beginoslheaders.h"
#include "OSL/genclosure.h"
#include "OSL/oslclosure.h"
#include "OSL/oslversion.h"
#include "foundation/platform/_endoslheaders.h"

// Standard headers.
#include <algorithm>

using namespace foundation;
using namespace renderer;
using namespace std;

using OSL::TypeDesc;

namespace renderer
{
namespace
{

    //
    // Global ustrings.
    //

    const OIIO::ustring g_beckmann_str("beckmann");
    const OIIO::ustring g_ggx_str("ggx");
    const OIIO::ustring g_gtr1_str("gtr1");
    const OIIO::ustring g_std_str("std");

    const OIIO::ustring g_standard_dipole_profile_str("standard_dipole");
    const OIIO::ustring g_better_dipole_profile_str("better_dipole");
    const OIIO::ustring g_directional_dipole_profile_str("directional_dipole");
    const OIIO::ustring g_normalized_diffusion_profile_str("normalized_diffusion");
    const OIIO::ustring g_gaussian_profile_str("gaussian");
    const OIIO::ustring g_randomwalk_profile_str("randomwalk");

    //
    // Closure functions.
    //

    typedef void (*convert_closure_fun)(
        CompositeSurfaceClosure&    composite_closure,
        const Basis3f&              shading_basis,
        const void*                 osl_params,
        const Color3f&              weight,
        Arena&                      arena);

    convert_closure_fun g_closure_convert_funs[NumClosuresIDs];

    void convert_closure_nop(
        CompositeSurfaceClosure&    composite_closure,
        const Basis3f&              shading_basis,
        const void*                 osl_params,
        const Color3f&              weight,
        Arena&                      arena)
    {
    }

    typedef int (*closure_get_modes)();

    closure_get_modes g_closure_get_modes_funs[NumClosuresIDs];

    int closure_no_modes()
    {
        return 0;
    }

    //
    // Closures.
    //

    struct AshikhminShirleyClosure
    {
        struct Params
        {
            OSL::Vec3   N;
            OSL::Vec3   T;
            OSL::Color3 diffuse_reflectance;
            OSL::Color3 glossy_reflectance;
            float       exponent_u;
            float       exponent_v;
            float       fresnel_multiplier;
        };

        static const char* name()
        {
            return "as_ashikhmin_shirley";
        }

        static ClosureID id()
        {
            return AshikhminShirleyID;
        }

        static int modes()
        {
            return ScatteringMode::Diffuse | ScatteringMode::Glossy;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_VECTOR_PARAM(Params, T),
                CLOSURE_COLOR_PARAM(Params, diffuse_reflectance),
                CLOSURE_COLOR_PARAM(Params, glossy_reflectance),
                CLOSURE_FLOAT_PARAM(Params, exponent_u),
                CLOSURE_FLOAT_PARAM(Params, exponent_v),
                CLOSURE_FLOAT_PARAM(Params, fresnel_multiplier),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            AshikhminBRDFInputValues* values =
                composite_closure.add_closure<AshikhminBRDFInputValues>(
                    id(),
                    shading_basis,
                    weight,
                    p->N,
                    p->T,
                    arena);

            values->m_rd.set(Color3f(p->diffuse_reflectance), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_rd_multiplier = 1.0f;
            values->m_rg.set(Color3f(p->glossy_reflectance), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_rg_multiplier = 1.0f;
            values->m_nu = max(p->exponent_u, 0.01f);
            values->m_nv = max(p->exponent_v, 0.01f);
            values->m_fr_multiplier = p->fresnel_multiplier;
        }
    };

    struct BackgroundClosure
    {
        struct Params
        {
        };

        static const char* name()
        {
            return "background";
        }

        static ClosureID id()
        {
            return BackgroundID;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);
        }
    };

    struct BlinnClosure
    {
        struct Params
        {
            OSL::Vec3       N;
            float           exponent;
            float           ior;
        };

        static const char* name()
        {
            return "as_blinn";
        }

        static ClosureID id()
        {
            return BlinnID;
        }

        static int modes()
        {
            return ScatteringMode::Glossy;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_FLOAT_PARAM(Params, exponent),
                CLOSURE_FLOAT_PARAM(Params, ior),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            BlinnBRDFInputValues* values =
                composite_closure.add_closure<BlinnBRDFInputValues>(
                    BlinnID,
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_exponent = max(p->exponent, 0.001f);
            values->m_ior = max(p->ior, 0.001f);
        }
    };

    struct DebugClosure
    {
        struct Params
        {
            OSL::ustring tag;
        };

        static const char* name()
        {
            return "debug";
        }

        static ClosureID id()
        {
            return DebugID;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_STRING_PARAM(Params, tag),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);
        }
    };

    struct DiffuseClosure
    {
        struct Params
        {
            OSL::Vec3   N;
        };

        static const char* name()
        {
            return "diffuse";
        }

        static ClosureID id()
        {
            return DiffuseID;
        }

        static int modes()
        {
            return ScatteringMode::Diffuse;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            OrenNayarBRDFInputValues* values =
                composite_closure.add_closure<OrenNayarBRDFInputValues>(
                    OrenNayarID,
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_reflectance.set(1.0f);
            values->m_reflectance_multiplier = 1.0f;
            values->m_roughness = 0.0f;
        }
    };

    struct DisneyClosure
    {
        struct Params
        {
            OSL::Vec3   N;
            OSL::Vec3   T;
            OSL::Color3 base_color;
            float       subsurface;
            float       metallic;
            float       specular;
            float       specular_tint;
            float       anisotropic;
            float       roughness;
            float       sheen;
            float       sheen_tint;
            float       clearcoat;
            float       clearcoat_gloss;
        };

        static const char* name()
        {
            return "as_disney";
        }

        static ClosureID id()
        {
            return DisneyID;
        }

        static int modes()
        {
            return ScatteringMode::Diffuse | ScatteringMode::Glossy;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_VECTOR_PARAM(Params, T),
                CLOSURE_COLOR_PARAM(Params, base_color),
                CLOSURE_FLOAT_PARAM(Params, subsurface),
                CLOSURE_FLOAT_PARAM(Params, metallic),
                CLOSURE_FLOAT_PARAM(Params, specular),
                CLOSURE_FLOAT_PARAM(Params, specular_tint),
                CLOSURE_FLOAT_PARAM(Params, anisotropic),
                CLOSURE_FLOAT_PARAM(Params, roughness),
                CLOSURE_FLOAT_PARAM(Params, sheen),
                CLOSURE_FLOAT_PARAM(Params, sheen_tint),
                CLOSURE_FLOAT_PARAM(Params, clearcoat),
                CLOSURE_FLOAT_PARAM(Params, clearcoat_gloss),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            DisneyBRDFInputValues* values =
                composite_closure.add_closure<DisneyBRDFInputValues>(
                    id(),
                    shading_basis,
                    weight,
                    p->N,
                    p->T,
                    arena);

            values->m_base_color.set(Color3f(p->base_color), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_subsurface = saturate(p->subsurface);
            values->m_metallic = saturate(p->metallic);
            values->m_specular = max(p->specular, 0.0f);
            values->m_specular_tint = saturate(p->specular_tint);
            values->m_anisotropic = clamp(p->anisotropic, -1.0f, 1.0f);
            values->m_roughness = clamp(p->roughness, 0.0001f, 1.0f);
            values->m_sheen = saturate(p->sheen);
            values->m_sheen_tint = saturate(p->sheen_tint);
            values->m_clearcoat = max(p->clearcoat, 0.0f);
            values->m_clearcoat_gloss = clamp(p->clearcoat_gloss, 0.0001f, 1.0f);
        }
    };

    struct EmissionClosure
    {
        struct Params
        {
        };

        static const char* name()
        {
            return "emission";
        }

        static ClosureID id()
        {
            return EmissionID;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);
        }

        static void convert_closure(
            CompositeEmissionClosure&   composite_closure,
            const void*                 osl_params,
            const Color3f&              weight,
            const float                 max_weight_component,
            Arena&                      arena)
        {
            DiffuseEDFInputValues* values =
                composite_closure.add_closure<DiffuseEDFInputValues>(
                    id(),
                    weight,
                    max_weight_component,
                    arena);

            values->m_radiance.set(Color3f(weight / max_weight_component), g_std_lighting_conditions, Spectrum::Illuminance);
            values->m_radiance_multiplier = max_weight_component;
            values->m_exposure = 0.0f;
        }
    };

    struct GlassClosure
    {
        struct Params
        {
            OSL::ustring    dist;
            OSL::Vec3       N;
            OSL::Vec3       T;
            OSL::Color3     surface_transmittance;
            OSL::Color3     reflection_tint;
            OSL::Color3     refraction_tint;
            float           roughness;
            float           highlight_falloff;
            float           anisotropy;
            float           ior;
            OSL::Color3     volume_transmittance;
            float           volume_transmittance_distance;
            float           energy_compensation;
        };

        static const char* name()
        {
            return "as_glass";
        }

        static ClosureID id()
        {
            return GlassID;
        }

        static int modes()
        {
            return ScatteringMode::Glossy | ScatteringMode::Specular;
        }

        static void prepare_closure(
            OSL::RendererServices*      render_services,
            int                         id,
            void*                       data)
        {
            // Initialize keyword parameter defaults.
            Params* params = new (data) Params();
            params->energy_compensation = 0.0f;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_STRING_PARAM(Params, dist),
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_VECTOR_PARAM(Params, T),
                CLOSURE_COLOR_PARAM(Params, surface_transmittance),
                CLOSURE_COLOR_PARAM(Params, reflection_tint),
                CLOSURE_COLOR_PARAM(Params, refraction_tint),
                CLOSURE_FLOAT_PARAM(Params, roughness),
                CLOSURE_FLOAT_PARAM(Params, highlight_falloff),
                CLOSURE_FLOAT_PARAM(Params, anisotropy),
                CLOSURE_FLOAT_PARAM(Params, ior),
                CLOSURE_COLOR_PARAM(Params, volume_transmittance),
                CLOSURE_FLOAT_PARAM(Params, volume_transmittance_distance),
                CLOSURE_FLOAT_KEYPARAM(Params, energy_compensation, "energy_compensation"),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, &prepare_closure, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;

            g_closure_get_modes_funs[id()] = &modes;
            g_closure_get_modes_funs[GlassBeckmannID] = &modes;
            g_closure_get_modes_funs[GlassGGXID] = &modes;
            g_closure_get_modes_funs[GlassSTDID] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            GlassBSDFInputValues* values;
            ClosureID cid;

            if (p->dist == g_ggx_str)
                cid = GlassGGXID;
            else if (p->dist == g_beckmann_str)
                cid = GlassBeckmannID;
            else if (p->dist == g_std_str)
                cid = GlassSTDID;
            else
            {
                string msg("invalid microfacet distribution function: ");
                msg += p->dist.c_str();
                throw ExceptionOSLRuntimeError(msg.c_str());
            }

            values =
                composite_closure.add_closure<GlassBSDFInputValues>(
                    cid,
                    shading_basis,
                    weight,
                    p->N,
                    p->T,
                    arena);

            values->m_surface_transmittance.set(Color3f(p->surface_transmittance), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_surface_transmittance_multiplier = 1.0f;
            values->m_reflection_tint.set(Color3f(p->reflection_tint), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_refraction_tint.set(Color3f(p->refraction_tint), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_roughness = max(p->roughness, 0.0001f);
            values->m_highlight_falloff = saturate(p->highlight_falloff);
            values->m_anisotropy = clamp(p->anisotropy, -1.0f, 1.0f);
            values->m_ior = max(p->ior, 0.001f);
            values->m_volume_transmittance.set(Color3f(p->volume_transmittance), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_volume_transmittance_distance = p->volume_transmittance_distance;
            values->m_energy_compensation = saturate(p->energy_compensation);
            composite_closure.add_ior(weight, values->m_ior);
        }
    };

    struct GlossyClosure
    {
        struct Params
        {
            OSL::ustring    dist;
            OSL::Vec3       N;
            OSL::Vec3       T;
            float           roughness;
            float           highlight_falloff;
            float           anisotropy;
            float           ior;
            float           energy_compensation;
        };

        static const char* name()
        {
            return "as_glossy";
        }

        static ClosureID id()
        {
            return GlossyID;
        }

        static int modes()
        {
            return ScatteringMode::Glossy | ScatteringMode::Specular;
        }

        static void prepare_closure(
            OSL::RendererServices*      render_services,
            int                         id,
            void*                       data)
        {
            // Initialize keyword parameter defaults.
            Params* params = new (data) Params();
            params->energy_compensation = 0.0f;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_STRING_PARAM(Params, dist),
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_VECTOR_PARAM(Params, T),
                CLOSURE_FLOAT_PARAM(Params, roughness),
                CLOSURE_FLOAT_PARAM(Params, highlight_falloff),
                CLOSURE_FLOAT_PARAM(Params, anisotropy),
                CLOSURE_FLOAT_PARAM(Params, ior),
                CLOSURE_FLOAT_KEYPARAM(Params, energy_compensation, "energy_compensation"),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, &prepare_closure, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;

            g_closure_get_modes_funs[id()] = &modes;
            g_closure_get_modes_funs[GlossyBeckmannID] = &modes;
            g_closure_get_modes_funs[GlossyGGXID] = &modes;
            g_closure_get_modes_funs[GlossySTDID] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            GlossyBRDFInputValues* values;
            ClosureID cid;

            const float roughness = saturate(p->roughness);
            const float highlight_falloff = saturate(p->highlight_falloff);
            const float ior = max(p->ior, 0.001f);
            float w = luminance(weight);

            if (p->dist == g_ggx_str)
            {
                cid = GlossyGGXID;
                w *= sample_weight<GGXMDF>(roughness, ior);
            }
            else if (p->dist == g_beckmann_str)
            {
                cid = GlossyBeckmannID;
                w *= sample_weight<BeckmannMDF>(roughness, ior);
            }
            else if (p->dist == g_std_str)
            {
                cid = GlossySTDID;
                w *= sample_weight_std(roughness, highlight_falloff, ior);
            }
            else
            {
                string msg("invalid microfacet distribution function: ");
                msg += p->dist.c_str();
                throw ExceptionOSLRuntimeError(msg.c_str());
            }

            values =
                composite_closure.add_closure<GlossyBRDFInputValues>(
                    cid,
                    shading_basis,
                    weight,
                    p->N,
                    p->T,
                    arena);
            composite_closure.override_closure_scalar_weight(w);

            values->m_reflectance.set(1.0f);
            values->m_reflectance_multiplier = 1.0f;
            values->m_roughness = roughness;
            values->m_highlight_falloff = highlight_falloff;
            values->m_anisotropy = clamp(p->anisotropy, -1.0f, 1.0f);
            values->m_ior = ior;
            values->m_fresnel_weight = 1.0f;
            values->m_energy_compensation = saturate(p->energy_compensation);
        }

        template <typename MDF>
        static float sample_weight(const float roughness, const float ior)
        {
            const float eavg = get_average_albedo(MDF(), roughness);
            const float favg = average_fresnel_reflectance_dielectric(ior);
            return eavg * favg;
        }

        static float sample_weight_std(
            const float roughness,
            const float highlight_falloff,
            const float ior)
        {
            const float eavg0 = get_average_albedo(GGXMDF(), roughness);
            const float eavg1 = get_average_albedo(BeckmannMDF(), roughness);
            const float eavg = lerp(eavg0, eavg1, highlight_falloff);
            const float favg = average_fresnel_reflectance_dielectric(ior);
            return eavg * favg;
        }
    };

    struct HoldoutClosure
    {
        struct Params
        {
        };

        static const char* name()
        {
            return "holdout";
        }

        static ClosureID id()
        {
            return HoldoutID;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);
        }
    };

    struct MetalClosure
    {
        struct Params
        {
            OSL::ustring    dist;
            OSL::Vec3       N;
            OSL::Vec3       T;
            OSL::Color3     normal_reflectance;
            OSL::Color3     edge_tint;
            float           roughness;
            float           highlight_falloff;
            float           anisotropy;
            float           energy_compensation;
        };

        static const char* name()
        {
            return "as_metal";
        }

        static ClosureID id()
        {
            return MetalID;
        }

        static int modes()
        {
            return ScatteringMode::Glossy | ScatteringMode::Specular;
        }

        static void prepare_closure(
            OSL::RendererServices*      render_services,
            int                         id,
            void*                       data)
        {
            // Initialize keyword parameter defaults.
            Params* params = new (data) Params();
            params->energy_compensation = 0.0f;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_STRING_PARAM(Params, dist),
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_VECTOR_PARAM(Params, T),
                CLOSURE_COLOR_PARAM(Params, normal_reflectance),
                CLOSURE_COLOR_PARAM(Params, edge_tint),
                CLOSURE_FLOAT_PARAM(Params, roughness),
                CLOSURE_FLOAT_PARAM(Params, highlight_falloff),
                CLOSURE_FLOAT_PARAM(Params, anisotropy),
                CLOSURE_FLOAT_KEYPARAM(Params, energy_compensation, "energy_compensation"),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, &prepare_closure, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;

            g_closure_get_modes_funs[id()] = &modes;
            g_closure_get_modes_funs[MetalBeckmannID] = &modes;
            g_closure_get_modes_funs[MetalGGXID] = &modes;
            g_closure_get_modes_funs[MetalSTDID] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            MetalBRDFInputValues* values;
            ClosureID cid;

            if (p->dist == g_ggx_str)
                cid = MetalGGXID;
            else if (p->dist == g_beckmann_str)
                cid = MetalBeckmannID;
            else if (p->dist == g_std_str)
                cid = MetalSTDID;
            else
            {
                string msg("invalid microfacet distribution function: ");
                msg += p->dist.c_str();
                throw ExceptionOSLRuntimeError(msg.c_str());
            }

            values =
                composite_closure.add_closure<MetalBRDFInputValues>(
                    cid,
                    shading_basis,
                    weight,
                    p->N,
                    p->T,
                    arena);

            values->m_normal_reflectance.set(Color3f(p->normal_reflectance), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_edge_tint.set(Color3f(p->edge_tint), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_reflectance_multiplier = 1.0f;
            values->m_roughness = max(p->roughness, 0.0f);
            values->m_highlight_falloff = saturate(p->highlight_falloff);
            values->m_anisotropy = clamp(p->anisotropy, -1.0f, 1.0f);
            values->m_energy_compensation = saturate(p->energy_compensation);
        }
    };

    struct OrenNayarClosure
    {
        struct Params
        {
            OSL::Vec3   N;
            float       roughness;
        };

        static const char* name()
        {
            return "oren_nayar";
        }

        static ClosureID id()
        {
            return OrenNayarID;
        }

        static int modes()
        {
            return ScatteringMode::Diffuse;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_FLOAT_PARAM(Params, roughness),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            OrenNayarBRDFInputValues* values =
                composite_closure.add_closure<OrenNayarBRDFInputValues>(
                    id(),
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_reflectance.set(1.0f);
            values->m_reflectance_multiplier = 1.0f;
            values->m_roughness = max(p->roughness, 0.0f);
        }
    };

    struct PhongClosure
    {
        struct Params
        {
            OSL::Vec3   N;
            float       exponent;
        };

        static const char* name()
        {
            return "phong";
        }

        static ClosureID id()
        {
            return PhongID;
        }

        static int modes()
        {
            return ScatteringMode::Glossy;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_FLOAT_PARAM(Params, exponent),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            AshikhminBRDFInputValues* values =
                composite_closure.add_closure<AshikhminBRDFInputValues>(
                    AshikhminShirleyID,
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_rd.set(1.0f);
            values->m_rd_multiplier = 1.0f;
            values->m_rg.set(1.0f);
            values->m_rg_multiplier = 1.0f;
            values->m_nu = max(p->exponent, 0.01f);
            values->m_nv = max(p->exponent, 0.01f);
            values->m_fr_multiplier = 1.0f;
        }
    };

    struct PlasticClosure
    {
        struct Params
        {
            OSL::ustring    dist;
            OSL::Vec3       N;
            OSL::Color3     specular_reflectance;
            float           specular_reflectance_multiplier;
            float           roughness;
            float           highlight_falloff;
            float           ior;
            OSL::Color3     diffuse_reflectance;
            float           diffuse_reflectance_multiplier;
            float           internal_scattering;
        };

        static const char* name()
        {
            return "as_plastic";
        }

        static ClosureID id()
        {
            return PlasticID;
        }

        static int modes()
        {
            return ScatteringMode::Diffuse | ScatteringMode::Glossy | ScatteringMode::Specular;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_STRING_PARAM(Params, dist),
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_COLOR_PARAM(Params, specular_reflectance),
                CLOSURE_FLOAT_PARAM(Params, specular_reflectance_multiplier),
                CLOSURE_FLOAT_PARAM(Params, roughness),
                CLOSURE_FLOAT_PARAM(Params, highlight_falloff),
                CLOSURE_FLOAT_PARAM(Params, ior),
                CLOSURE_COLOR_PARAM(Params, diffuse_reflectance),
                CLOSURE_FLOAT_PARAM(Params, diffuse_reflectance_multiplier),
                CLOSURE_FLOAT_PARAM(Params, internal_scattering),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;

            g_closure_get_modes_funs[id()] = &modes;
            g_closure_get_modes_funs[PlasticBeckmannID] = &modes;
            g_closure_get_modes_funs[PlasticGGXID] = &modes;
            g_closure_get_modes_funs[PlasticGTR1ID] = &modes;
            g_closure_get_modes_funs[PlasticSTDID] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            PlasticBRDFInputValues* values;
            ClosureID cid;

            if (p->dist == g_beckmann_str)
                cid = PlasticBeckmannID;
            else if (p->dist == g_ggx_str)
                cid = PlasticGGXID;
            else if (p->dist == g_gtr1_str)
                cid = PlasticGTR1ID;
            else if (p->dist == g_std_str)
                cid = PlasticSTDID;
            else
            {
                string msg("invalid microfacet distribution function: ");
                msg += p->dist.c_str();
                throw ExceptionOSLRuntimeError(msg.c_str());
            }

            values =
                composite_closure.add_closure<PlasticBRDFInputValues>(
                    cid,
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_specular_reflectance.set(Color3f(p->specular_reflectance), g_std_lighting_conditions,
            Spectrum::Reflectance);
            values->m_specular_reflectance_multiplier = max(p->specular_reflectance_multiplier, 0.0f);
            values->m_roughness = clamp(p->roughness, 0.0001f, 1.0f);
            values->m_highlight_falloff = saturate(p->highlight_falloff);
            values->m_ior = max(p->ior, 0.001f);
            values->m_diffuse_reflectance.set(Color3f(p->diffuse_reflectance), g_std_lighting_conditions,
            Spectrum::Reflectance);
            values->m_diffuse_reflectance_multiplier = max(p->diffuse_reflectance_multiplier, 0.0f);
            values->m_internal_scattering = max(p->internal_scattering, 0.0f);
        }
    };

    struct ReflectionClosure
    {
        struct Params
        {
            OSL::Vec3       N;
            float           ior;
        };

        static const char* name()
        {
            return "reflection";
        }

        static ClosureID id()
        {
            return ReflectionID;
        }

        static int modes()
        {
            return ScatteringMode::Specular;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_FLOAT_PARAM(Params, ior),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            GlossyBRDFInputValues* values =
                composite_closure.add_closure<GlossyBRDFInputValues>(
                    GlossyBeckmannID,
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_reflectance.set(1.0f);
            values->m_reflectance_multiplier = 1.0f;
            values->m_roughness = 0.0f;
            values->m_anisotropy = 0.0f;
            values->m_ior = max(p->ior, 0.001f);
            values->m_energy_compensation = 0.0f;
        }
    };

    struct SheenClosure
    {
        struct Params
        {
            OSL::Vec3 N;
        };

        static const char* name()
        {
            return "as_sheen";
        }

        static ClosureID id()
        {
            return SheenID;
        }

        static int modes()
        {
            return ScatteringMode::Diffuse;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            SheenBRDFInputValues* values =
                composite_closure.add_closure<SheenBRDFInputValues>(
                    id(),
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_reflectance.set(1.0f);
            values->m_reflectance_multiplier = 1.0f;
        }
    };

    struct SubsurfaceClosure
    {
        struct Params
        {
            OSL::ustring    profile;
            OSL::Vec3       N;
            OSL::Color3     reflectance;
            OSL::Color3     mean_free_path;
            float           ior;
            float           fresnel_weight;
        };

        static const char* name()
        {
            return "as_subsurface";
        }

        static ClosureID id()
        {
            return SubsurfaceID;
        }

        static void prepare_closure(
            OSL::RendererServices*      render_services,
            int                         id,
            void*                       data)
        {
            // Initialize keyword parameter defaults.
            Params* params = new (data) Params();
            params->fresnel_weight = 1.0f;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_STRING_PARAM(Params, profile),
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_COLOR_PARAM(Params, reflectance),
                CLOSURE_COLOR_PARAM(Params, mean_free_path),
                CLOSURE_FLOAT_PARAM(Params, ior),
                CLOSURE_FLOAT_KEYPARAM(Params, fresnel_weight, "fresnel_weight"),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, &prepare_closure, nullptr);
        }

        static void convert_closure(
            CompositeSubsurfaceClosure& composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            if (p->profile == g_normalized_diffusion_profile_str)
            {
                NormalizedDiffusionBSSRDFInputValues* values =
                    composite_closure.add_closure<NormalizedDiffusionBSSRDFInputValues>(
                        SubsurfaceNormalizedDiffusionID,
                        shading_basis,
                        weight,
                        p->N,
                        arena);

                copy_parameters(p, values);
            }
            else if (p->profile == g_gaussian_profile_str)
            {
                GaussianBSSRDFInputValues* values =
                    composite_closure.add_closure<GaussianBSSRDFInputValues>(
                        SubsurfaceGaussianID,
                        shading_basis,
                        weight,
                        p->N,
                        arena);

                copy_parameters(p, values);
            }
            else if (p->profile == g_randomwalk_profile_str)
            {
                RandomWalkBSSRDFInputValues* values =
                    composite_closure.add_closure<RandomWalkBSSRDFInputValues>(
                        SubsurfaceRandomWalkID,
                        shading_basis,
                        weight,
                        p->N,
                        arena);

                copy_parameters(p, values);
                values->m_zero_scattering_weight = 1.0f;
            }
            else
            {
                DipoleBSSRDFInputValues* values;

                if (p->profile == g_better_dipole_profile_str)
                {
                    values =
                        composite_closure.add_closure<DipoleBSSRDFInputValues>(
                            SubsurfaceBetterDipoleID,
                            shading_basis,
                            weight,
                            p->N,
                            arena);
                }
                else if (p->profile == g_standard_dipole_profile_str)
                {
                    values =
                        composite_closure.add_closure<DipoleBSSRDFInputValues>(
                            SubsurfaceStandardDipoleID,
                            shading_basis,
                            weight,
                            p->N,
                            arena);
                }
                else if (p->profile == g_directional_dipole_profile_str)
                {
                    values =
                        composite_closure.add_closure<DipoleBSSRDFInputValues>(
                            SubsurfaceDirectionalDipoleID,
                            shading_basis,
                            weight,
                            p->N,
                            arena);
                }
                else
                {
                    string msg = "unknown subsurface profile: ";
                    msg += p->profile.c_str();
                    throw ExceptionOSLRuntimeError(msg.c_str());
                }

                copy_parameters(p, values);
            }
        }

        template <typename InputValues>
        static void copy_parameters(
            const Params*               p,
            InputValues*                values)
        {
            values->m_weight = 1.0f;
            values->m_reflectance.set(Color3f(p->reflectance), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_reflectance_multiplier = 1.0f;
            values->m_mfp.set(Color3f(p->mean_free_path), g_std_lighting_conditions, Spectrum::Reflectance);
            values->m_mfp_multiplier = 1.0f;
            values->m_ior = p->ior;
            values->m_fresnel_weight = saturate(p->fresnel_weight);
        }
    };

    struct TranslucentClosure
    {
        struct Params
        {
            OSL::Vec3 N;
        };

        static const char* name()
        {
            return "translucent";
        }

        static ClosureID id()
        {
            return TranslucentID;
        }

        static int modes()
        {
            return ScatteringMode::Diffuse;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_VECTOR_PARAM(Params, N),
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);

            g_closure_convert_funs[id()] = &convert_closure;
            g_closure_get_modes_funs[id()] = &modes;
        }

        static void convert_closure(
            CompositeSurfaceClosure&    composite_closure,
            const Basis3f&              shading_basis,
            const void*                 osl_params,
            const Color3f&              weight,
            Arena&                      arena)
        {
            const Params* p = static_cast<const Params*>(osl_params);

            DiffuseBTDFInputValues* values =
                composite_closure.add_closure<DiffuseBTDFInputValues>(
                    id(),
                    shading_basis,
                    weight,
                    p->N,
                    arena);

            values->m_transmittance.set(1.0f);
            values->m_transmittance_multiplier = 1.0f;
        }
    };

    struct TransparentClosure
    {
        struct Params
        {
        };

        static const char* name()
        {
            return "transparent";
        }

        static ClosureID id()
        {
            return TransparentID;
        }

        static void register_closure(OSLShadingSystem& shading_system)
        {
            const OSL::ClosureParam params[] =
            {
                CLOSURE_FINISH_PARAM(Params)
            };

            shading_system.register_closure(name(), id(), params, nullptr, nullptr);
        }
    };
}


//
// CompositeClosure class implementation.
//

CompositeClosure::CompositeClosure()
  : m_closure_count(0)
{
}

void CompositeClosure::compute_closure_shading_basis(
    const Vector3f& normal,
    const Basis3f&  original_shading_basis)
{
    const float normal_square_norm = square_norm(normal);
    if APPLESEED_LIKELY(normal_square_norm != 0.0f)
    {
        const float rcp_normal_norm = 1.0f / sqrt(normal_square_norm);
        m_bases[m_closure_count] =
            Basis3f(
                normal * rcp_normal_norm,
                original_shading_basis.get_tangent_u());
    }
    else
    {
        // Fallback to the original shading basis if the normal is zero.
        m_bases[m_closure_count] = original_shading_basis;
    }
}

void CompositeClosure::compute_closure_shading_basis(
    const Vector3f& normal,
    const Vector3f& tangent,
    const Basis3f&  original_shading_basis)
{
    const float tangent_square_norm = square_norm(tangent);
    if APPLESEED_LIKELY(tangent_square_norm != 0.0f)
    {
        const float normal_square_norm = square_norm(normal);
        if APPLESEED_LIKELY(normal_square_norm != 0.0f)
        {
            const float rcp_normal_norm = 1.0f / sqrt(normal_square_norm);
            const float rcp_tangent_norm = 1.0f / sqrt(tangent_square_norm);
            m_bases[m_closure_count] =
                Basis3f(
                    normal * rcp_normal_norm,
                    tangent * rcp_tangent_norm);
        }
        else
        {
            // Fallback to the original shading basis if the normal is zero.
            m_bases[m_closure_count] = original_shading_basis;
        }
    }
    else
    {
        // If the tangent is zero, ignore it.
        // This can happen when using the isotropic microfacet closure overloads, for example.
        compute_closure_shading_basis(normal, original_shading_basis);
    }
}

template <typename InputValues>
InputValues* CompositeClosure::add_closure(
    const ClosureID             closure_type,
    const Basis3f&              original_shading_basis,
    const Color3f&              weight,
    const Vector3f&             normal,
    Arena&                      arena)
{
    return do_add_closure<InputValues>(
        closure_type,
        original_shading_basis,
        weight,
        normal,
        false,
        Vector3f(0.0f),
        arena);
}

template <typename InputValues>
InputValues* CompositeClosure::add_closure(
    const ClosureID             closure_type,
    const Basis3f&              original_shading_basis,
    const Color3f&              weight,
    const Vector3f&             normal,
    const Vector3f&             tangent,
    Arena&                      arena)
{
    return do_add_closure<InputValues>(
        closure_type,
        original_shading_basis,
        weight,
        normal,
        true,
        tangent,
        arena);
}

template <typename InputValues>
InputValues* CompositeClosure::do_add_closure(
    const ClosureID             closure_type,
    const Basis3f&              original_shading_basis,
    const Color3f&              weight,
    const Vector3f&             normal,
    const bool                  has_tangent,
    const Vector3f&             tangent,
    Arena&                      arena)
{
    // Make sure we have enough space.
    if APPLESEED_UNLIKELY(get_closure_count() >= MaxClosureEntries)
    {
        throw ExceptionOSLRuntimeError(
            "maximum number of closures in osl shader group exceeded");
    }

    // We use the luminance of the weight as the BSDF weight.
    const float w = luminance(weight);
    assert(w > 0.0f);

    m_weights[m_closure_count].set(weight, g_std_lighting_conditions, Spectrum::Reflectance);
    m_scalar_weights[m_closure_count] = w;

    if (!has_tangent)
        compute_closure_shading_basis(normal, original_shading_basis);
    else compute_closure_shading_basis(normal, tangent, original_shading_basis);

    m_closure_types[m_closure_count] = closure_type;

    InputValues* values = arena.allocate<InputValues>();
    m_input_values[m_closure_count] = values;

    ++m_closure_count;

    return values;
}

void CompositeClosure::compute_pdfs(float pdfs[MaxClosureEntries])
{
    const size_t closure_count = get_closure_count();

    float total_weight = 0.0f;
    for (size_t i = 0; i < closure_count; ++i)
    {
        pdfs[i] = m_scalar_weights[i];
        total_weight += pdfs[i];
    }

    if (total_weight != 0.0f)
    {
        const float rcp_total_weight = 1.0f / total_weight;

        for (size_t i = 0; i < closure_count; ++i)
            pdfs[i] *= rcp_total_weight;
    }
}


//
// CompositeSurfaceClosure class implementation.
//

CompositeSurfaceClosure::CompositeSurfaceClosure(
    const Basis3f&              original_shading_basis,
    const OSL::ClosureColor*    ci,
    Arena&                      arena)
  : m_ior_count(0)
{
    process_closure_tree(ci, original_shading_basis, Color3f(1.0f), arena);

    if (m_ior_count == 0)
    {
        m_ior_count = 1;
        m_iors[0] = 1.0f;
        return;
    }

    // Build the IOR CDF in place if needed.
    if (m_ior_count > 1)
    {
        float total_weight = m_ior_cdf[0];
        for (size_t i = 1; i < m_ior_count; ++i)
        {
            total_weight += m_ior_cdf[i];
            m_ior_cdf[i] = total_weight;
        }

        const float rcp_total_weight = 1.0f / total_weight;

        for (size_t i = 0; i < m_ior_count - 1; ++i)
            m_ior_cdf[i] *= rcp_total_weight;

        m_ior_cdf[m_ior_count - 1] = 1.0f;
    }
}

int CompositeSurfaceClosure::compute_pdfs(
    const int                   modes,
    float                       pdfs[MaxClosureEntries]) const
{
    memset(pdfs, 0, sizeof(float) * MaxClosureEntries);

    int num_closures = 0;
    float sum_weights = 0.0f;

    for (size_t i = 0, e = get_closure_count(); i < e; ++i)
    {
        const ClosureID cid = m_closure_types[i];
        const int closure_modes = g_closure_get_modes_funs[cid]();

        if (closure_modes & modes)
        {
            pdfs[i] = m_scalar_weights[i];
            sum_weights += m_scalar_weights[i];
            ++num_closures;
        }
        else
            pdfs[i] = 0.0f;
    }

    if (sum_weights != 0.0f)
    {
        const float rcp_sum_weights = 1.0f / sum_weights;
        for (size_t i = 0, e = get_closure_count(); i < e; ++i)
            pdfs[i] *= rcp_sum_weights;
    }

    return num_closures;
}

size_t CompositeSurfaceClosure::choose_closure(
    const float                 w,
    const size_t                num_closures,
    float                       pdfs[MaxClosureEntries]) const
{
    assert(num_closures > 0);
    assert(num_closures < MaxClosureEntries);

    return sample_pdf_linear_search(pdfs, num_closures, w);
}

void CompositeSurfaceClosure::add_ior(
    const foundation::Color3f&  weight,
    const float                 ior)
{
    // We use the luminance of the weight as the IOR weight.
    const float w = luminance(weight);
    assert(w > 0.0f);

    m_iors[m_ior_count] = ior;
    m_ior_cdf[m_ior_count] = w;
    ++m_ior_count;
}

float CompositeSurfaceClosure::choose_ior(const float w) const
{
    assert(m_ior_count > 0);

    if APPLESEED_LIKELY(m_ior_count == 1)
        return m_iors[0];

    const size_t index = sample_cdf_linear_search(m_ior_cdf, w);
    return m_iors[index];
}

void CompositeSurfaceClosure::process_closure_tree(
    const OSL::ClosureColor*    closure,
    const Basis3f&              original_shading_basis,
    const Color3f&              weight,
    Arena&                      arena)
{
    if (closure == nullptr)
        return;

    switch (closure->id)
    {
      case OSL::ClosureColor::MUL:
        {
            const OSL::ClosureMul* c = reinterpret_cast<const OSL::ClosureMul*>(closure);
            const Color3f w = weight * Color3f(c->weight);
            process_closure_tree(c->closure, original_shading_basis, w, arena);
        }
        break;

      case OSL::ClosureColor::ADD:
        {
            const OSL::ClosureAdd* c = reinterpret_cast<const OSL::ClosureAdd*>(closure);
            process_closure_tree(c->closureA, original_shading_basis, weight, arena);
            process_closure_tree(c->closureB, original_shading_basis, weight, arena);
        }
        break;

      default:
        {
            const OSL::ClosureComponent* c = reinterpret_cast<const OSL::ClosureComponent*>(closure);
            const Color3f w = weight * Color3f(c->w);

            if (luminance(w) > 0.0f)
                g_closure_convert_funs[c->id](*this, original_shading_basis, c->data(), w, arena);
        }
        break;
    }
}


//
// CompositeSubsurfaceClosure class implementation.
//

CompositeSubsurfaceClosure::CompositeSubsurfaceClosure(
    const Basis3f&              original_shading_basis,
    const OSL::ClosureColor*    ci,
    Arena&                      arena)
{
    process_closure_tree(ci, original_shading_basis, Color3f(1.0f), arena);
    compute_pdfs(m_pdfs);
}

size_t CompositeSubsurfaceClosure::choose_closure(const float w) const
{
    assert(get_closure_count() > 0);
    return sample_pdf_linear_search(m_pdfs, get_closure_count(), w);
}

void CompositeSubsurfaceClosure::process_closure_tree(
    const OSL::ClosureColor*    closure,
    const Basis3f&              original_shading_basis,
    const foundation::Color3f&  weight,
    Arena&                      arena)
{
    if (closure == nullptr)
        return;

    switch (closure->id)
    {
      case OSL::ClosureColor::MUL:
        {
            const OSL::ClosureMul* c = reinterpret_cast<const OSL::ClosureMul*>(closure);
            process_closure_tree(c->closure, original_shading_basis, weight * Color3f(c->weight), arena);
        }
        break;

      case OSL::ClosureColor::ADD:
        {
            const OSL::ClosureAdd* c = reinterpret_cast<const OSL::ClosureAdd*>(closure);
            process_closure_tree(c->closureA, original_shading_basis, weight, arena);
            process_closure_tree(c->closureB, original_shading_basis, weight, arena);
        }
        break;

      default:
        {
            const OSL::ClosureComponent* c = reinterpret_cast<const OSL::ClosureComponent*>(closure);

            if (c->id == SubsurfaceID)
            {
                const Color3f w = weight * Color3f(c->w);
                if (luminance(w) > 0.0f)
                {
                    SubsurfaceClosure::convert_closure(
                        *this,
                        original_shading_basis,
                        c->data(),
                        w,
                        arena);
                }
            }
        }
        break;
    }
}


//
// CompositeEmissionClosure class implementation.
//

CompositeEmissionClosure::CompositeEmissionClosure(
    const OSL::ClosureColor*    ci,
    Arena&                      arena)
{
    process_closure_tree(ci, Color3f(1.0f), arena);
    compute_pdfs(m_pdfs);
}

size_t CompositeEmissionClosure::choose_closure(const float w) const
{
    assert(get_closure_count() > 0);
    return sample_pdf_linear_search(m_pdfs, get_closure_count(), w);
}

template <typename InputValues>
InputValues* CompositeEmissionClosure::add_closure(
    const ClosureID             closure_type,
    const Color3f&              weight,
    const float                 max_weight_component,
    Arena&                      arena)
{
    // Make sure we have enough space.
    if APPLESEED_UNLIKELY(get_closure_count() >= MaxClosureEntries)
    {
        throw ExceptionOSLRuntimeError(
            "maximum number of closures in osl shader group exceeded");
    }

    m_closure_types[m_closure_count] = closure_type;
    m_weights[m_closure_count].set(weight, g_std_lighting_conditions, Spectrum::Reflectance);
    m_pdfs[m_closure_count] = max_weight_component;

    InputValues* values = arena.allocate<InputValues>();
    m_input_values[m_closure_count] = values;

    ++m_closure_count;

    return values;
}

void CompositeEmissionClosure::process_closure_tree(
    const OSL::ClosureColor*    closure,
    const Color3f&              weight,
    Arena&                      arena)
{
    if (closure == nullptr)
        return;

    switch (closure->id)
    {
      case OSL::ClosureColor::MUL:
        {
            const OSL::ClosureMul* c = reinterpret_cast<const OSL::ClosureMul*>(closure);
            process_closure_tree(c->closure, weight * Color3f(c->weight), arena);
        }
        break;

      case OSL::ClosureColor::ADD:
        {
            const OSL::ClosureAdd* c = reinterpret_cast<const OSL::ClosureAdd*>(closure);
            process_closure_tree(c->closureA, weight, arena);
            process_closure_tree(c->closureB, weight, arena);
        }
        break;

      default:
        {
            const OSL::ClosureComponent* c = reinterpret_cast<const OSL::ClosureComponent*>(closure);

            const Color3f w = weight * Color3f(c->w);
            const float max_weight_component = max_value(w);

            if (max_weight_component > 0.0f)
            {
                if (c->id == EmissionID)
                {
                    EmissionClosure::convert_closure(
                        *this,
                        c->data(),
                        w,
                        max_weight_component,
                        arena);
                }
            }
        }
        break;
    }
}


//
// Utility functions implementation.
//

namespace
{
    Color3f do_process_closure_id_tree(
        const OSL::ClosureColor*    closure,
        const int                   closure_id)
    {
        if (closure)
        {
            switch (closure->id)
            {
              case OSL::ClosureColor::MUL:
                {
                    const OSL::ClosureMul* c = reinterpret_cast<const OSL::ClosureMul*>(closure);
                    return Color3f(c->weight) * do_process_closure_id_tree(c->closure, closure_id);
                }
                break;

              case OSL::ClosureColor::ADD:
                {
                    const OSL::ClosureAdd* c = reinterpret_cast<const OSL::ClosureAdd*>(closure);
                    return do_process_closure_id_tree(c->closureA, closure_id) +
                           do_process_closure_id_tree(c->closureB, closure_id);
                }
                break;

              default:
                {
                    const OSL::ClosureComponent* c = reinterpret_cast<const OSL::ClosureComponent*>(closure);

                    if (c->id == closure_id)
                        return Color3f(c->w);
                    else return Color3f(0.0f);
                }
                break;
            }
        }

        return Color3f(0.0f);
    }
}

void process_transparency_tree(const OSL::ClosureColor* ci, Alpha& alpha)
{
    // Convert from transparency to opacity.
    const float transparency = saturate(luminance(do_process_closure_id_tree(ci, TransparentID)));
    alpha.set(1.0f - transparency);
}

float process_holdout_tree(const OSL::ClosureColor* ci)
{
    return saturate(luminance(do_process_closure_id_tree(ci, HoldoutID)));
}

Color3f process_background_tree(const OSL::ClosureColor* ci)
{
    return do_process_closure_id_tree(ci, BackgroundID);
}

namespace
{
    template <typename ClosureType>
    void register_closure(OSLShadingSystem& shading_system)
    {
        ClosureType::register_closure(shading_system);
        RENDERER_LOG_DEBUG("registered osl closure %s.", ClosureType::name());
    }
}

void register_closures(OSLShadingSystem& shading_system)
{
    for (size_t i = 0; i < NumClosuresIDs; ++i)
    {
        g_closure_convert_funs[i] = &convert_closure_nop;
        g_closure_get_modes_funs[i] = &closure_no_modes;
    }

    register_closure<AshikhminShirleyClosure>(shading_system);
    register_closure<BackgroundClosure>(shading_system);
    register_closure<BlinnClosure>(shading_system);
    register_closure<DebugClosure>(shading_system);
    register_closure<DiffuseClosure>(shading_system);
    register_closure<DisneyClosure>(shading_system);
    register_closure<EmissionClosure>(shading_system);
    register_closure<GlassClosure>(shading_system);
    register_closure<GlossyClosure>(shading_system);
    register_closure<HoldoutClosure>(shading_system);
    register_closure<MetalClosure>(shading_system);
    register_closure<OrenNayarClosure>(shading_system);
    register_closure<PhongClosure>(shading_system);
    register_closure<PlasticClosure>(shading_system);
    register_closure<ReflectionClosure>(shading_system);
    register_closure<SheenClosure>(shading_system);
    register_closure<SubsurfaceClosure>(shading_system);
    register_closure<TranslucentClosure>(shading_system);
    register_closure<TransparentClosure>(shading_system);
}

}   // namespace renderer
