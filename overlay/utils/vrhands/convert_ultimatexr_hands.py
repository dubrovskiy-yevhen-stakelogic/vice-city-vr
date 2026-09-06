#!/usr/bin/env python3
"""Convert the MIT-licensed UltimateXR BigHands assets to reVC VR hand meshes.

The runtime deliberately does not depend on Unity or an FBX parser. FBX2glTF is
used once to obtain embedded glTF files, then this script bakes four authored
skin poses (open, grip, trigger and grip+trigger) into a compact morph mesh.

Requires Python 3, Pillow and NumPy. Example:

  python tools/convert_ultimatexr_hands.py \
      --ultimatexr path/to/ultimatexr-unity \
      --gltf-dir path/to/converted/BigHands \
      --output-dir gamefiles/models/vrhands
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import re
import struct
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

import numpy as np
from PIL import Image


MAGIC = b"UXRH"
VERSION = 1
COMPONENT_FORMATS = {
    5120: "b",
    5121: "B",
    5122: "h",
    5123: "H",
    5125: "I",
    5126: "f",
}
TYPE_COMPONENTS = {
    "SCALAR": 1,
    "VEC2": 2,
    "VEC3": 3,
    "VEC4": 4,
    "MAT4": 16,
}


def matrix_identity() -> np.ndarray:
    return np.identity(4, dtype=np.float64)


def quaternion_matrix(q: Sequence[float]) -> np.ndarray:
    x, y, z, w = (float(v) for v in q)
    length = math.sqrt(x * x + y * y + z * z + w * w)
    if length < 1.0e-12:
        return matrix_identity()
    x, y, z, w = x / length, y / length, z / length, w / length
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    result = matrix_identity()
    result[:3, :3] = np.array(
        [
            [1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)],
            [2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)],
            [2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)],
        ],
        dtype=np.float64,
    )
    return result


def node_local_matrix(node: dict, rotation_override: Sequence[float] | None) -> np.ndarray:
    if "matrix" in node and rotation_override is None:
        return np.array(node["matrix"], dtype=np.float64).reshape((4, 4), order="F")
    translation = node.get("translation", (0.0, 0.0, 0.0))
    rotation = rotation_override if rotation_override is not None else node.get("rotation", (0.0, 0.0, 0.0, 1.0))
    scale = node.get("scale", (1.0, 1.0, 1.0))
    result = quaternion_matrix(rotation)
    result[:3, 0] *= float(scale[0])
    result[:3, 1] *= float(scale[1])
    result[:3, 2] *= float(scale[2])
    result[:3, 3] = np.array(translation, dtype=np.float64)
    return result


class EmbeddedGltf:
    def __init__(self, path: Path):
        self.path = path
        self.doc = json.loads(path.read_text(encoding="utf-8"))
        uri = self.doc["buffers"][0]["uri"]
        if not uri.startswith("data:") or ";base64," not in uri:
            raise ValueError(f"{path}: expected an embedded base64 buffer")
        self.buffer = base64.b64decode(uri.split(",", 1)[1])

    def accessor(self, index: int) -> np.ndarray:
        accessor = self.doc["accessors"][index]
        view = self.doc["bufferViews"][accessor["bufferView"]]
        component_type = accessor["componentType"]
        fmt = COMPONENT_FORMATS[component_type]
        component_count = TYPE_COMPONENTS[accessor["type"]]
        component_size = struct.calcsize("<" + fmt)
        packed_size = component_size * component_count
        stride = int(view.get("byteStride", packed_size))
        start = int(view.get("byteOffset", 0)) + int(accessor.get("byteOffset", 0))
        values = []
        unpack = struct.Struct("<" + fmt * component_count).unpack_from
        for element in range(int(accessor["count"])):
            values.append(unpack(self.buffer, start + element * stride))
        if component_type == 5126:
            dtype = np.float64
        elif component_type in (5120, 5122):
            dtype = np.int32
        else:
            dtype = np.uint32
        result = np.asarray(values, dtype=dtype)
        if accessor.get("normalized") and component_type != 5126:
            if component_type == 5121:
                result = result.astype(np.float64) / 255.0
            elif component_type == 5123:
                result = result.astype(np.float64) / 65535.0
            else:
                raise ValueError(f"Unsupported normalized component type {component_type}")
        return result


def parse_fixed_pose(asset_path: Path, clip_name: str) -> Dict[str, Tuple[float, float, float, float]]:
    text = asset_path.read_text(encoding="utf-8")
    objects = re.split(r"^--- !u!74 ", text, flags=re.MULTILINE)[1:]
    clip = None
    for obj in objects:
        match = re.search(r"^  m_Name: (.+)$", obj, flags=re.MULTILINE)
        if match and match.group(1).strip() == clip_name:
            clip = obj
            break
    if clip is None:
        raise ValueError(f"{asset_path}: AnimationClip '{clip_name}' was not found")

    rotations: Dict[str, Tuple[float, float, float, float]] = {}
    curve_pattern = r"  - curve:\n(?P<body>.*?)(?=\n  - curve:|\n  m_CompressedRotationCurves:)"
    value_pattern = r"value: \{x: ([^,]+), y: ([^,]+), z: ([^,]+), w: ([^}]+)\}"
    for curve_match in re.finditer(curve_pattern, clip, flags=re.DOTALL):
        body = curve_match.group("body")
        path_match = re.search(r"    path: (.+)", body)
        value_match = re.search(value_pattern, body)
        if path_match is None or value_match is None:
            continue
        leaf = path_match.group(1).strip().split("/")[-1]
        leaf = leaf.replace("AvatarMaleHands_", "")
        ux, uy, uz, uw = (float(value) for value in value_match.groups())
        # FBX2glTF converts Unity's left-handed coordinate system by flipping
        # Y/Z quaternion components. Matching the bind-pose node rotations
        # verifies this conversion for every authored finger chain.
        rotations[leaf] = (ux, -uy, -uz, uw)
    if not rotations:
        raise ValueError(f"{asset_path}: no rotation curves in '{clip_name}'")
    return rotations


def build_parent_table(nodes: Sequence[dict]) -> List[int]:
    parents = [-1] * len(nodes)
    for parent, node in enumerate(nodes):
        for child in node.get("children", ()):
            parents[int(child)] = parent
    return parents


def global_matrices(nodes: Sequence[dict], parents: Sequence[int], pose: Dict[str, Sequence[float]]) -> List[np.ndarray]:
    result: List[np.ndarray | None] = [None] * len(nodes)

    def evaluate(index: int) -> np.ndarray:
        existing = result[index]
        if existing is not None:
            return existing
        node = nodes[index]
        local = node_local_matrix(node, pose.get(node.get("name", "")))
        parent = parents[index]
        value = evaluate(parent) @ local if parent >= 0 else local
        result[index] = value
        return value

    for index in range(len(nodes)):
        evaluate(index)
    return [value for value in result if value is not None]


def skin_pose(
    gltf: EmbeddedGltf,
    mesh_node_index: int,
    skin_index: int,
    positions: np.ndarray,
    normals: np.ndarray,
    joints: np.ndarray,
    weights: np.ndarray,
    pose: Dict[str, Sequence[float]],
) -> Tuple[np.ndarray, np.ndarray]:
    nodes = gltf.doc["nodes"]
    parents = build_parent_table(nodes)
    globals_ = global_matrices(nodes, parents, pose)
    mesh_inverse = np.linalg.inv(globals_[mesh_node_index])
    skin = gltf.doc["skins"][skin_index]
    inverse_bind_raw = gltf.accessor(skin["inverseBindMatrices"])
    inverse_binds = [values.reshape((4, 4), order="F") for values in inverse_bind_raw]
    joint_matrices = [
        mesh_inverse @ globals_[node_index] @ inverse_binds[index]
        for index, node_index in enumerate(skin["joints"])
    ]

    output_positions = np.zeros_like(positions, dtype=np.float64)
    output_normals = np.zeros_like(normals, dtype=np.float64)
    for vertex in range(len(positions)):
        source_position = np.array((positions[vertex, 0], positions[vertex, 1], positions[vertex, 2], 1.0))
        source_normal = normals[vertex]
        total_weight = 0.0
        for influence in range(4):
            weight = float(weights[vertex, influence])
            if weight <= 1.0e-8:
                continue
            matrix = joint_matrices[int(joints[vertex, influence])]
            output_positions[vertex] += (matrix @ source_position)[:3] * weight
            output_normals[vertex] += (matrix[:3, :3] @ source_normal) * weight
            total_weight += weight
        if total_weight <= 1.0e-8:
            output_positions[vertex] = positions[vertex]
            output_normals[vertex] = normals[vertex]
        else:
            output_positions[vertex] /= total_weight
            length = np.linalg.norm(output_normals[vertex])
            if length > 1.0e-12:
                output_normals[vertex] /= length
    return output_positions, output_normals


def combine_pose(
    open_pose: Dict[str, Sequence[float]],
    closed_pose: Dict[str, Sequence[float]],
    close_index: bool,
    close_grip: bool,
) -> Dict[str, Sequence[float]]:
    result = dict(open_pose)
    for name, rotation in closed_pose.items():
        is_index = name.startswith("Index_")
        if (is_index and close_index) or (not is_index and close_grip):
            result[name] = rotation
    return result


def find_hand_primitive(gltf: EmbeddedGltf, side: str) -> Tuple[int, int, dict]:
    expected_mesh_name = f"Hand{side}"
    for node_index, node in enumerate(gltf.doc["nodes"]):
        if "mesh" not in node or "skin" not in node:
            continue
        mesh = gltf.doc["meshes"][node["mesh"]]
        if mesh.get("name") == expected_mesh_name:
            primitives = mesh["primitives"]
            if len(primitives) != 1:
                raise ValueError(f"{gltf.path}: expected one primitive in {expected_mesh_name}")
            return node_index, int(node["skin"]), primitives[0]
    raise ValueError(f"{gltf.path}: {expected_mesh_name} skinned mesh was not found")


def convert_hand(gltf_path: Path, default_asset: Path, grab_asset: Path, side: str, output_path: Path) -> None:
    gltf = EmbeddedGltf(gltf_path)
    mesh_node_index, skin_index, primitive = find_hand_primitive(gltf, side)
    attributes = primitive["attributes"]
    positions = gltf.accessor(attributes["POSITION"])
    normals = gltf.accessor(attributes["NORMAL"])
    uvs = gltf.accessor(attributes["TEXCOORD_0"])
    joints = gltf.accessor(attributes["JOINTS_0"])
    weights = gltf.accessor(attributes["WEIGHTS_0"])
    indices = gltf.accessor(primitive["indices"]).reshape(-1)

    if len(positions) > 65535 or int(indices.max()) > 65535:
        raise ValueError(f"{gltf_path}: mesh exceeds the 16-bit immediate-mode index limit")

    open_pose = parse_fixed_pose(default_asset, f"Fixed {side}")
    closed_pose = parse_fixed_pose(grab_asset, f"Fixed {side}")
    pose_inputs = [
        combine_pose(open_pose, closed_pose, False, False),
        combine_pose(open_pose, closed_pose, False, True),
        combine_pose(open_pose, closed_pose, True, False),
        combine_pose(open_pose, closed_pose, True, True),
    ]
    pose_vertices = [
        skin_pose(gltf, mesh_node_index, skin_index, positions, normals, joints, weights, pose)
        for pose in pose_inputs
    ]

    # GetVrHandBasis already handles anatomical handedness. Canonicalise the
    # FBX's left-hand mirror here so both assets use +Y as the same palm normal.
    if side == "Left":
        for skinned_positions, skinned_normals in pose_vertices:
            skinned_positions[:, 1] *= -1.0
            skinned_normals[:, 1] *= -1.0

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as output:
        output.write(MAGIC)
        output.write(struct.pack("<III", VERSION, len(positions), len(indices)))
        for vertex in range(len(positions)):
            values: List[float] = []
            for skinned_positions, _ in pose_vertices:
                values.extend(float(value) for value in skinned_positions[vertex])
            for _, skinned_normals in pose_vertices:
                values.extend(float(value) for value in skinned_normals[vertex])
            values.extend((float(uvs[vertex, 0]), float(uvs[vertex, 1])))
            output.write(struct.pack("<" + "f" * 26, *values))
        output.write(struct.pack("<" + "H" * len(indices), *(int(index) for index in indices)))

    all_open = pose_vertices[0][0]
    minimum = all_open.min(axis=0)
    maximum = all_open.max(axis=0)
    print(
        f"{side}: {len(positions)} vertices, {len(indices) // 3} triangles, "
        f"open bounds {minimum.tolist()} .. {maximum.tolist()} -> {output_path}"
    )


def smoothstep(edge0: float, edge1: float, value: np.ndarray) -> np.ndarray:
    t = np.clip((value - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def bake_albedo(texture_dir: Path, output_path: Path) -> None:
    mask1 = np.asarray(Image.open(texture_dir / "BigHandsTex_Mask1.png").convert("RGB"), dtype=np.float32) / 255.0
    mask2 = np.asarray(Image.open(texture_dir / "BigHandsTex_Mask2.png").convert("RGB"), dtype=np.float32) / 255.0
    mask3 = np.asarray(Image.open(texture_dir / "BigHandsTex_Mask3.png").convert("RGB"), dtype=np.float32) / 255.0

    skin = np.array((0.80, 0.60, 0.46666667), dtype=np.float32)
    palm = np.array((0.83137256, 0.6431373, 0.52156866), dtype=np.float32)
    nail = np.array((0.93, 0.68, 0.66), dtype=np.float32)
    line_colour = np.array((0.72, 0.31, 0.23), dtype=np.float32)

    palm_mask = smoothstep(0.08, 0.80, mask1[:, :, 1])[:, :, None]
    nail_mask = smoothstep(0.10, 0.75, mask1[:, :, 2])[:, :, None]
    line_mask = np.clip(mask2[:, :, 0] * 0.22, 0.0, 0.16)[:, :, None]
    albedo = skin[None, None, :] * (1.0 - palm_mask) + palm[None, None, :] * palm_mask
    albedo = albedo * (1.0 - line_mask) + line_colour[None, None, :] * line_mask
    albedo = albedo * (1.0 - nail_mask) + nail[None, None, :] * nail_mask

    # Preserve the authored AO and very subtle skin variation without baking
    # Unity lighting into the texture. This keeps the hands readable at VR scale.
    ao = 0.82 + 0.18 * mask1[:, :, 0:1]
    variation = 0.97 + (mask3[:, :, 1:2] - 0.5) * 0.05
    albedo = np.clip(albedo * ao * variation, 0.0, 1.0)
    image = Image.fromarray((albedo * 255.0 + 0.5).astype(np.uint8), mode="RGB")
    image = image.resize((1024, 1024), Image.Resampling.LANCZOS)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_path, optimize=True)
    print(f"Baked Type 3 skin albedo -> {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ultimatexr", type=Path, required=True, help="Root of the ultimatexr-unity checkout")
    parser.add_argument("--gltf-dir", type=Path, required=True, help="Directory containing embedded BigHand*.gltf files")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    big_hands = args.ultimatexr / "Runtime" / "Art" / "Avatars" / "BigHands"
    poses = big_hands / "HandPoses"
    convert_hand(
        args.gltf_dir / "BigHandLeft.gltf",
        poses / "Default.asset",
        poses / "Grab.asset",
        "Left",
        args.output_dir / "BigHandLeft.uxrh",
    )
    convert_hand(
        args.gltf_dir / "BigHandRight.gltf",
        poses / "Default.asset",
        poses / "Grab.asset",
        "Right",
        args.output_dir / "BigHandRight.uxrh",
    )
    bake_albedo(big_hands / "Textures", args.output_dir / "BigHandsAlbedo.png")


if __name__ == "__main__":
    main()
