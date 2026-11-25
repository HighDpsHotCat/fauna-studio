"""Read the default scene's triangle count without loading vertex buffers."""

import json
from pathlib import Path
import struct


def triangle_count(path):
    path = Path(path)
    with path.open("rb") as stream:
        header = stream.read(20)
        if len(header) != 20:
            raise ValueError("GLB 헤더가 손상되었습니다.")
        magic, version, length, chunk_size, kind = struct.unpack("<4sIII4s", header)
        if (magic != b"glTF" or version != 2 or kind != b"JSON" or
                length != path.stat().st_size or chunk_size > length - 20):
            raise ValueError("glTF 2.0 바이너리(.glb) 파일을 선택해 주세요.")
        try:
            doc = json.loads(stream.read(chunk_size))
            counts = []
            for mesh in doc.get("meshes", []):
                count = 0
                for part in mesh["primitives"]:
                    index = part.get("indices", part["attributes"]["POSITION"])
                    vertices = doc["accessors"][index]["count"]
                    mode = part.get("mode", 4)
                    if mode == 4:
                        count += vertices // 3
                    elif mode in (5, 6):
                        count += max(0, vertices - 2)
                counts.append(count)
            pending = list(doc["scenes"][doc.get("scene", 0)].get("nodes", []))
            seen = set()
            total = 0
            while pending:
                index = pending.pop()
                if index in seen:
                    raise ValueError("GLB 노드 계층이 올바르지 않습니다.")
                seen.add(index)
                node = doc["nodes"][index]
                if "mesh" in node:
                    total += counts[node["mesh"]]
                pending.extend(node.get("children", []))
        except (KeyError, IndexError, TypeError, AttributeError, json.JSONDecodeError) as error:
            raise ValueError("GLB의 장면 또는 메시 정보를 읽을 수 없습니다.") from error
    if total < 1:
        raise ValueError("선택한 GLB에 삼각형 메시가 없습니다.")
    return total
