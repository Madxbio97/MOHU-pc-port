#include "mohu/tsp_scene.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace mohu {
namespace {

constexpr std::size_t header_size = 72U;
constexpr std::uint32_t vertex_mask = 0x1fffU;
constexpr std::uint32_t strip_end = 0x1fffU;
constexpr std::uint32_t node_end = 0x1fff1fffU;

struct Header {
  std::uint16_t id{};
  std::uint16_t version{};
  std::int32_t node_count{};
  std::uint32_t node_offset{};
  std::int32_t declared_face_count{};
  std::uint32_t face_offset{};
  std::int32_t position_count{};
  std::uint32_t position_offset{};
  std::int32_t unused_b_count{};
  std::uint32_t unused_b_offset{};
  std::int32_t color_count{};
  std::uint32_t color_offset{};
  std::int32_t unused_c_count{};
  std::uint32_t unused_c_offset{};
  std::int32_t dynamic_count{};
  std::uint32_t dynamic_offset{};
  std::uint32_t collision_offset{};
  std::int32_t material_count{};
  std::uint32_t material_offset{};
};

class Reader {
public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool contains(std::uint64_t offset,
                              std::uint64_t size) const noexcept {
    return offset <= bytes_.size() && size <= bytes_.size() - offset;
  }
  [[nodiscard]] std::uint8_t u8(std::size_t offset) const noexcept {
    return bytes_[offset];
  }
  [[nodiscard]] std::uint16_t u16(std::size_t offset) const noexcept {
    return static_cast<std::uint16_t>(bytes_[offset]) |
           (static_cast<std::uint16_t>(bytes_[offset + 1U]) << 8U);
  }
  [[nodiscard]] std::int16_t i16(std::size_t offset) const noexcept {
    return static_cast<std::int16_t>(u16(offset));
  }
  [[nodiscard]] std::uint32_t u32(std::size_t offset) const noexcept {
    return static_cast<std::uint32_t>(u16(offset)) |
           (static_cast<std::uint32_t>(u16(offset + 2U)) << 16U);
  }
  [[nodiscard]] std::int32_t i32(std::size_t offset) const noexcept {
    return static_cast<std::int32_t>(u32(offset));
  }

private:
  std::span<const std::uint8_t> bytes_;
};

class SceneLoader {
public:
  explicit SceneLoader(std::span<const std::uint8_t> bytes) : reader_(bytes) {}

  [[nodiscard]] TspSceneLoadResult run() {
    if (!readHeader() || !readFixedSections() || !readNodes() ||
        !resolveChildren()) {
      result_.scene = {};
    }
    return std::move(result_);
  }

private:
  [[nodiscard]] bool fail(TspSceneError error,
                          std::size_t offset) noexcept {
    if (result_.error == TspSceneError::none) {
      result_.error = error;
      result_.error_offset = offset;
    }
    return false;
  }
  [[nodiscard]] bool validCount(std::int32_t count, std::uint32_t offset,
                                std::size_t stride) const noexcept {
    return count >= 0 &&
           reader_.contains(offset, static_cast<std::uint64_t>(count) *
                                        static_cast<std::uint64_t>(stride));
  }

  [[nodiscard]] bool readHeader() {
    if (!reader_.contains(0U, header_size)) {
      return fail(TspSceneError::truncated_header, 0U);
    }
    header_.id = reader_.u16(0U);
    header_.version = reader_.u16(2U);
    header_.node_count = reader_.i32(4U);
    header_.node_offset = reader_.u32(8U);
    header_.declared_face_count = reader_.i32(12U);
    header_.face_offset = reader_.u32(16U);
    header_.position_count = reader_.i32(20U);
    header_.position_offset = reader_.u32(24U);
    header_.unused_b_count = reader_.i32(28U);
    header_.unused_b_offset = reader_.u32(32U);
    header_.color_count = reader_.i32(36U);
    header_.color_offset = reader_.u32(40U);
    header_.unused_c_count = reader_.i32(44U);
    header_.unused_c_offset = reader_.u32(48U);
    header_.dynamic_count = reader_.i32(52U);
    header_.dynamic_offset = reader_.u32(56U);
    header_.collision_offset = reader_.u32(60U);
    header_.material_count = reader_.i32(64U);
    header_.material_offset = reader_.u32(68U);

    if (header_.version != 3U) {
      return fail(TspSceneError::unsupported_version, 2U);
    }
    if (header_.node_count < 0 || header_.declared_face_count < 0 ||
        header_.position_count < 0 || header_.color_count < 0 ||
        header_.material_count < 0 || header_.unused_b_count < 0 ||
        header_.unused_c_count < 0 || header_.dynamic_count < 0) {
      return fail(TspSceneError::invalid_count, 4U);
    }
    if (header_.color_count < header_.position_count) {
      return fail(TspSceneError::invalid_count, 36U);
    }
    result_.scene.id = header_.id;
    result_.scene.version = header_.version;
    return true;
  }

  [[nodiscard]] bool readFixedSections() {
    if (!validCount(header_.position_count, header_.position_offset, 8U)) {
      return fail(TspSceneError::invalid_section, header_.position_offset);
    }
    if (!validCount(header_.color_count, header_.color_offset, 4U)) {
      return fail(TspSceneError::invalid_section, header_.color_offset);
    }
    if (!validCount(header_.material_count, header_.material_offset, 12U)) {
      return fail(TspSceneError::invalid_section, header_.material_offset);
    }

    auto &scene = result_.scene;
    scene.positions.reserve(static_cast<std::size_t>(header_.position_count));
    scene.colors.reserve(static_cast<std::size_t>(header_.color_count));
    scene.materials.reserve(static_cast<std::size_t>(header_.material_count));
    scene.nodes.reserve(static_cast<std::size_t>(header_.node_count));
    for (std::int32_t index{}; index < header_.position_count; ++index) {
      const auto offset = static_cast<std::size_t>(header_.position_offset) +
                          static_cast<std::size_t>(index) * 8U;
      const auto padding = reader_.i16(offset + 6U);
      if (padding != 104 && padding != 105) {
        return fail(TspSceneError::invalid_vertex_padding, offset + 6U);
      }
      scene.positions.push_back({reader_.i16(offset), reader_.i16(offset + 2U),
                                 reader_.i16(offset + 4U)});
    }
    for (std::int32_t index{}; index < header_.color_count; ++index) {
      const auto offset = static_cast<std::size_t>(header_.color_offset) +
                          static_cast<std::size_t>(index) * 4U;
      scene.colors.push_back({reader_.u8(offset), reader_.u8(offset + 1U),
                              reader_.u8(offset + 2U),
                              reader_.u8(offset + 3U)});
    }
    for (std::int32_t index{}; index < header_.material_count; ++index) {
      const auto offset = static_cast<std::size_t>(header_.material_offset) +
                          static_cast<std::size_t>(index) * 12U;
      TspMaterial material{};
      material.uv[0] = {reader_.u8(offset), reader_.u8(offset + 1U)};
      material.clut = reader_.u16(offset + 2U);
      material.uv[1] = {reader_.u8(offset + 4U), reader_.u8(offset + 5U)};
      material.texture_page = reader_.u16(offset + 6U);
      material.uv[2] = {reader_.u8(offset + 8U), reader_.u8(offset + 9U)};
      scene.materials.push_back(material);
    }
    return true;
  }

  [[nodiscard]] bool appendTriangle(std::array<std::uint16_t, 3U> indices,
                                    std::uint16_t material_index,
                                    std::uint32_t source_offset,
                                    bool swap_winding) {
    if (std::any_of(indices.begin(), indices.end(), [&](std::uint16_t index) {
          return index >= result_.scene.positions.size();
        })) {
      return fail(TspSceneError::invalid_position_index,
                  header_.face_offset + source_offset);
    }
    if (material_index >= result_.scene.materials.size()) {
      return fail(TspSceneError::invalid_material_index,
                  header_.face_offset + source_offset);
    }
    const auto &material = result_.scene.materials[material_index];
    auto uv = material.uv;
    if (swap_winding) {
      std::swap(indices[1], indices[2]);
      std::swap(uv[1], uv[2]);
    }
    result_.scene.triangles.push_back(
        {indices, uv, material_index, source_offset});
    return true;
  }

  [[nodiscard]] bool readLeafFaces(std::uint32_t base_offset,
                                   std::uint32_t face_count) {
    const auto absolute = static_cast<std::uint64_t>(header_.face_offset) +
                          static_cast<std::uint64_t>(base_offset);
    if (!reader_.contains(absolute, 8U)) {
      return fail(TspSceneError::invalid_face_stream,
                  static_cast<std::size_t>(absolute));
    }
    auto cursor = static_cast<std::size_t>(absolute);
    std::uint32_t emitted{};
    std::array<std::uint16_t, 3U> strip{};
    while (emitted < face_count) {
      if (!reader_.contains(cursor, 8U)) {
        return fail(TspSceneError::invalid_face_stream, cursor);
      }
      const auto source_offset =
          static_cast<std::uint32_t>(cursor - header_.face_offset);
      const auto packed = reader_.u32(cursor);
      strip = {static_cast<std::uint16_t>(packed & vertex_mask),
               static_cast<std::uint16_t>((packed >> 16U) & vertex_mask),
               static_cast<std::uint16_t>(reader_.u16(cursor + 4U) &
                                          vertex_mask)};
      if (!appendTriangle(strip, reader_.u16(cursor + 6U), source_offset,
                          false)) {
        return false;
      }
      ++emitted;
      cursor += 8U;
      for (;;) {
        if (!reader_.contains(cursor, 4U)) {
          return fail(TspSceneError::invalid_face_stream, cursor);
        }
        const auto marker_offset =
            static_cast<std::uint32_t>(cursor - header_.face_offset);
        const auto marker = reader_.u32(cursor);
        cursor += 4U;
        if ((marker & vertex_mask) == strip_end) {
          if (marker == node_end && emitted != face_count) {
            return fail(TspSceneError::invalid_face_stream, cursor - 4U);
          }
          break;
        }
        if (emitted >= face_count) {
          return fail(TspSceneError::invalid_face_stream, cursor - 4U);
        }
        if ((marker & 0x8000U) != 0U) {
          strip[0] = strip[2];
        } else {
          strip[0] = strip[1];
          strip[1] = strip[2];
        }
        strip[2] = static_cast<std::uint16_t>(marker & vertex_mask);
        if (!appendTriangle(strip, static_cast<std::uint16_t>(marker >> 16U),
                            marker_offset, (marker & 0x4000U) != 0U)) {
          return false;
        }
        ++emitted;
      }
    }
    return true;
  }

  [[nodiscard]] bool readNodes() {
    if (!reader_.contains(header_.node_offset, 0U) ||
        header_.node_offset > header_.face_offset) {
      return fail(TspSceneError::invalid_section, header_.node_offset);
    }
    auto cursor = static_cast<std::size_t>(header_.node_offset);
    node_file_offsets_.reserve(static_cast<std::size_t>(header_.node_count));
    node_child_offsets_.reserve(static_cast<std::size_t>(header_.node_count));
    for (std::int32_t index{}; index < header_.node_count; ++index) {
      if (!reader_.contains(cursor, 28U) || cursor + 28U > header_.face_offset) {
        return fail(TspSceneError::invalid_node, cursor);
      }
      const auto file_offset = static_cast<std::uint32_t>(cursor);
      TspNode node{};
      node.minimum = {reader_.i16(cursor), reader_.i16(cursor + 2U),
                      reader_.i16(cursor + 4U)};
      node.maximum = {reader_.i16(cursor + 6U), reader_.i16(cursor + 8U),
                      reader_.i16(cursor + 10U)};
      const auto face_count = reader_.i32(cursor + 12U);
      const auto base_data = reader_.i32(cursor + 24U);
      if (face_count < 0 || (face_count != 0 && base_data < 0)) {
        return fail(TspSceneError::invalid_node, cursor + 12U);
      }
      node.first_triangle =
          static_cast<std::uint32_t>(result_.scene.triangles.size());
      node.triangle_count = static_cast<std::uint32_t>(face_count);
      cursor += 28U;
      std::array<std::int32_t, 3U> child_offsets{-1, -1, -1};
      if (face_count == 0) {
        if (!reader_.contains(cursor, 8U) || cursor + 8U > header_.face_offset) {
          return fail(TspSceneError::invalid_node, cursor);
        }
        child_offsets[0] = reader_.i32(cursor);
        child_offsets[1] = reader_.i32(cursor + 4U);
        child_offsets[2] = base_data > 0 ? base_data : -1;
        cursor += 8U;
      } else if (!readLeafFaces(static_cast<std::uint32_t>(base_data),
                                static_cast<std::uint32_t>(face_count))) {
        return false;
      }
      node_file_offsets_.push_back(file_offset);
      node_child_offsets_.push_back(child_offsets);
      result_.scene.nodes.push_back(node);
    }
    return true;
  }

  [[nodiscard]] bool resolveChildren() {
    std::vector<std::pair<std::uint32_t, std::int32_t>> lookup;
    lookup.reserve(node_file_offsets_.size());
    for (std::size_t index{}; index < node_file_offsets_.size(); ++index) {
      lookup.emplace_back(node_file_offsets_[index],
                          static_cast<std::int32_t>(index));
    }
    std::sort(lookup.begin(), lookup.end());
    for (std::size_t node_index{}; node_index < result_.scene.nodes.size();
         ++node_index) {
      for (std::size_t child{}; child < 3U; ++child) {
        const auto relative = node_child_offsets_[node_index][child];
        if (relative < 0) {
          continue;
        }
        const auto absolute = static_cast<std::uint64_t>(header_.node_offset) +
                              static_cast<std::uint32_t>(relative);
        if (absolute > std::numeric_limits<std::uint32_t>::max()) {
          return fail(TspSceneError::invalid_child_reference,
                      node_file_offsets_[node_index]);
        }
        const auto key = static_cast<std::uint32_t>(absolute);
        const auto found = std::lower_bound(
            lookup.begin(), lookup.end(), key,
            [](const auto &entry, std::uint32_t value) {
              return entry.first < value;
            });
        if (found == lookup.end() || found->first != key) {
          return fail(TspSceneError::invalid_child_reference,
                      node_file_offsets_[node_index]);
        }
        result_.scene.nodes[node_index].children[child] = found->second;
      }
    }
    return true;
  }

  Reader reader_;
  Header header_{};
  TspSceneLoadResult result_{};
  std::vector<std::uint32_t> node_file_offsets_;
  std::vector<std::array<std::int32_t, 3U>> node_child_offsets_;
};

} // namespace

TspSceneLoadResult
loadTspScene(std::span<const std::uint8_t> bytes) noexcept {
  try {
    return SceneLoader{bytes}.run();
  } catch (const std::bad_alloc &) {
    TspSceneLoadResult result{};
    result.error = TspSceneError::out_of_memory;
    return result;
  }
}

const char *tspSceneErrorMessage(TspSceneError error) noexcept {
  switch (error) {
  case TspSceneError::none:
    return "none";
  case TspSceneError::truncated_header:
    return "truncated header";
  case TspSceneError::unsupported_version:
    return "unsupported TSP version";
  case TspSceneError::invalid_count:
    return "invalid element count";
  case TspSceneError::invalid_section:
    return "invalid section range";
  case TspSceneError::invalid_node:
    return "invalid node";
  case TspSceneError::invalid_face_stream:
    return "invalid face stream";
  case TspSceneError::invalid_position_index:
    return "invalid position index";
  case TspSceneError::invalid_material_index:
    return "invalid material index";
  case TspSceneError::invalid_child_reference:
    return "invalid child reference";
  case TspSceneError::invalid_vertex_padding:
    return "invalid vertex padding";
  case TspSceneError::out_of_memory:
    return "out of memory";
  }
  return "unknown error";
}

} // namespace mohu
