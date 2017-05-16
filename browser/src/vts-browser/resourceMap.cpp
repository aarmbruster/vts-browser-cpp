#include <vts-libs/vts/meshio.hpp>
#include <vts/map.hpp>

#include "map.hpp"
#include "image.hpp"

namespace vts
{

MetaTile::MetaTile(const std::string &name) : Resource(name, true),
    vtslibs::vts::MetaTile(vtslibs::vts::TileId(), 0)
{}

void MetaTile::load(MapImpl *)
{
    LOG(info2) << "Loading meta tile '" << impl->name << "'";
    detail::Wrapper w(impl->contentData);
    *(vtslibs::vts::MetaTile*)this
            = vtslibs::vts::loadMetaTile(w, 5, impl->name);
    impl->ramMemoryCost = this->size() * sizeof(vtslibs::vts::MetaNode);
}

NavTile::NavTile(const std::string &name) : Resource(name, true)
{}

void NavTile::load(MapImpl *)
{
    LOG(info2) << "Loading navigation tile '" << impl->name << "'";
    GpuTextureSpec spec;
    decodeImage(impl->contentData, spec.buffer,
                spec.width, spec.height, spec.components);
    if (spec.width != 256 || spec.height != 256 || spec.components != 1)
        LOGTHROW(err1, std::runtime_error) << "invalid navtile image";
    data.resize(256 * 256);
    memcpy(data.data(), spec.buffer.data(), 256 * 256);
}

vec2 NavTile::sds2px(const vec2 &point, const math::Extents2 &extents)
{
    return vecFromUblas<vec2>(vtslibs::vts::NavTile::sds2px(
                                  vecToUblas<math::Point2>(point), extents));
}

MeshPart::MeshPart() : textureLayer(0), surfaceReference(0),
    internalUv(false), externalUv(false)
{}

MeshAggregate::MeshAggregate(const std::string &name) : Resource(name, true)
{}

namespace {
const mat4 findNormToPhys(const math::Extents3 &extents)
{
    vec3 u = vecFromUblas<vec3>(extents.ur);
    vec3 l = vecFromUblas<vec3>(extents.ll);
    vec3 d = (u - l) * 0.5;
    vec3 c = (u + l) * 0.5;
    mat4 sc = scaleMatrix(d(0), d(1), d(2));
    mat4 tr = translationMatrix(c);
    return tr * sc;
}
}

void MeshAggregate::load(MapImpl *base)
{
    LOG(info2) << "Loading (aggregated) mesh '" << impl->name << "'";
    
    detail::Wrapper w(impl->contentData);
    vtslibs::vts::NormalizedSubMesh::list meshes = vtslibs::vts::
            loadMeshProperNormalized(w, impl->name);

    submeshes.clear();
    submeshes.reserve(meshes.size());

    for (uint32 mi = 0, me = meshes.size(); mi != me; mi++)
    {
        vtslibs::vts::SubMesh &m = meshes[mi].submesh;

        char tmp[10];
        sprintf(tmp, "%d", mi);
        std::shared_ptr<GpuMesh> gm
                = std::dynamic_pointer_cast<GpuMesh>
                (base->callbacks.createMesh
                 (impl->name + "#" + tmp));

        uint32 vertexSize = sizeof(vec3f);
        if (m.tc.size())
            vertexSize += sizeof(vec2f);
        if (m.etc.size())
            vertexSize += sizeof(vec2f);

        GpuMeshSpec spec;
        spec.verticesCount = m.faces.size() * 3;
        spec.vertices.allocate(spec.verticesCount * vertexSize);
        uint32 offset = 0;

        { // vertices
            spec.attributes[0].enable = true;
            spec.attributes[0].components = 3;
            vec3f *b = (vec3f*)spec.vertices.data();
            for (vtslibs::vts::Point3u32 f : m.faces)
            {
                for (uint32 j = 0; j < 3; j++)
                {
                    vec3 p3 = vecFromUblas<vec3>(m.vertices[f[j]]);
                    *b++ = p3.cast<float>();
                }
            }
            offset += m.faces.size() * sizeof(vec3f) * 3;
        }

        if (!m.tc.empty())
        { // internal, separated
            spec.attributes[1].enable = true;
            spec.attributes[1].components = 2;
            spec.attributes[1].offset = offset;
            vec2f *b = (vec2f*)(spec.vertices.data() + offset);
            for (vtslibs::vts::Point3u32 f : m.facesTc)
                for (uint32 j = 0; j < 3; j++)
                    *b++ = vecFromUblas<vec2f>(m.tc[f[j]]);
            offset += m.faces.size() * sizeof(vec2f) * 3;
        }

        if (!m.etc.empty())
        { // external, interleaved
            spec.attributes[2].enable = true;
            spec.attributes[2].components = 2;
            spec.attributes[2].offset = offset;
            vec2f *b = (vec2f*)(spec.vertices.data() + offset);
            for (vtslibs::vts::Point3u32 f : m.faces)
                for (uint32 j = 0; j < 3; j++)
                    *b++ = vecFromUblas<vec2f>(m.etc[f[j]]);
            offset += m.faces.size() * sizeof(vec2f) * 3;
        }

        gm->loadMesh(spec);
        gm->impl->state = ResourceImpl::State::ready;

        MeshPart part;
        part.renderable = gm;
        part.normToPhys = findNormToPhys(meshes[mi].extents);
        part.internalUv = spec.attributes[1].enable;
        part.externalUv = spec.attributes[2].enable;
        part.textureLayer = m.textureLayer ? *m.textureLayer : 0;
        part.surfaceReference = m.surfaceReference;
        submeshes.push_back(part);
    }

    impl->gpuMemoryCost = 0;
    impl->ramMemoryCost = meshes.size() * sizeof(MeshPart);
    for (auto &&it : submeshes)
    {
        impl->gpuMemoryCost += it.renderable->impl->gpuMemoryCost;
        impl->ramMemoryCost += it.renderable->impl->ramMemoryCost;
    }
}

BoundMetaTile::BoundMetaTile(const std::string &name) : Resource(name, true)
{}

void BoundMetaTile::load(MapImpl *)
{
    LOG(info2) << "Loading bound meta tile '" << impl->name << "'";
    
    Buffer buffer = std::move(impl->contentData);
    GpuTextureSpec spec;
    decodeImage(buffer, spec.buffer,
                spec.width, spec.height, spec.components);
    if (spec.buffer.size() != sizeof(flags))
        LOGTHROW(err1, std::runtime_error)
                << "bound meta tile has invalid resolution";
    memcpy(flags, spec.buffer.data(), spec.buffer.size());
    impl->ramMemoryCost = spec.buffer.size();
}

BoundMaskTile::BoundMaskTile(const std::string &name) : Resource(name, true)
{}

void BoundMaskTile::load(MapImpl *base)
{
    LOG(info2) << "Loading bound mask tile '" << impl->name << "'";
    
    if (!texture)
        texture = std::dynamic_pointer_cast<GpuTexture>(
                    base->callbacks.createTexture(impl->name + "#tex"));
    
    Buffer buffer = std::move(impl->contentData);
    GpuTextureSpec spec;
    decodeImage(buffer, spec.buffer,
                spec.width, spec.height, spec.components);
    texture->loadTexture(spec);
    impl->gpuMemoryCost = texture->impl->gpuMemoryCost;
    impl->ramMemoryCost = texture->impl->ramMemoryCost;
}

} // namespace vts