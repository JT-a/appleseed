
//
// This source file is part of appleseed.
// Visit https://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2013 Francois Beaune, Jupiter Jazz Limited
// Copyright (c) 2014-2018 Francois Beaune, The appleseedhq Organization
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

#ifndef APPLESEED_FOUNDATION_MESH_MESHBUILDERBASE_H
#define APPLESEED_FOUNDATION_MESH_MESHBUILDERBASE_H

// appleseed.foundation headers.
#include "foundation/math/vector.h"
#include "foundation/mesh/imeshbuilder.h"
#include "foundation/platform/compiler.h"

// appleseed.main headers.
#include "main/dllsymbol.h"

// Standard headers.
#include <cstddef>

namespace foundation
{

//
// A mesh builder that does nothing, typically serves as a base class.
//

class APPLESEED_DLLSYMBOL MeshBuilderBase
  : public IMeshBuilder
{
  public:
    void begin_mesh(const char* name) override
    {
    }

    size_t push_vertex(const Vector3d& v) override
    {
        return 0;
    }

    size_t push_vertex_normal(const Vector3d& v) override
    {
        return 0;
    }

    size_t push_tex_coords(const Vector2d& v) override
    {
        return 0;
    }

    size_t push_material_slot(const char* name) override
    {
        return 0;
    }

    void begin_face(const size_t vertex_count) override
    {
    }

    void set_face_vertices(const size_t vertices[]) override
    {
    }

    void set_face_vertex_normals(const size_t vertex_normals[]) override
    {
    }

    void set_face_vertex_tex_coords(const size_t tex_coords[]) override
    {
    }

    void set_face_material(const size_t material) override
    {
    }

    void end_face() override
    {
    }

    void end_mesh() override
    {
    }
};

}       // namespace foundation

#endif  // !APPLESEED_FOUNDATION_MESH_MESHBUILDERBASE_H
