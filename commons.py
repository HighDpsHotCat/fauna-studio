"""Find reference photos via Wikidata taxon names and Wikimedia Commons."""

from dataclasses import dataclass
from functools import lru_cache
from html.parser import HTMLParser
from io import BytesIO
import re

import requests


WIKIDATA = "https://www.wikidata.org/w/api.php"
COMMONS = "https://commons.wikimedia.org/w/api.php"
HEADERS = {"User-Agent": "FaunaStudio/1.0 (local desktop wildlife photo reference tool; Wikimedia Commons API)"}
NOT_PHOTO = re.compile(
    r"\b(paintings?|painted|drawings?|illustrations?|lithographs?|engravings?|"
    r"maps?|postage stamps?|diagrams?|sculptures?|taxidermy|skeletons?|coins?)\b", re.I
)


class _Text(HTMLParser):
    def __init__(self):
        super().__init__()
        self.parts = []

    def handle_data(self, data):
        self.parts.append(data)


def plain(html):
    parser = _Text()
    parser.feed(html)
    return " ".join(" ".join(parser.parts).split())


@dataclass(frozen=True)
class Taxon:
    scientific: str
    korean: str = ""
    qid: str = ""
    category: str = ""


@dataclass(frozen=True)
class Photo:
    page_id: int
    title: str
    thumbnail: str
    page_url: str
    author: str
    license: str
    basis: str


@dataclass(frozen=True)
class Gallery:
    taxon: Taxon
    photos: tuple[Photo, ...]


def _api(session, url, **params):
    response = session.get(url, params=dict(format="json", formatversion=2, **params), timeout=(5, 17))
    response.raise_for_status()
    result = response.json()
    if "error" in result:
        raise RuntimeError(result["error"].get("info", "위키미디어 API 오류"))
    return result


def _values(entity, prop):
    return [claim["mainsnak"]["datavalue"]["value"]
            for claim in entity.get("claims", {}).get(prop, [])
            if claim.get("rank") != "deprecated" and "datavalue" in claim["mainsnak"]]


def _entities(session, ids):
    return _api(session, WIKIDATA, action="wbgetentities", ids="|".join(ids),
                props="claims|labels", languages="ko").get("entities", {})


def resolve_taxon(session, scientific):
    hits = _api(session, WIKIDATA, action="wbsearchentities", search=scientific,
                language="en", type="item", limit=5).get("search", [])
    if hits:
        entities = _entities(session, [hit["id"] for hit in hits])
        for hit in hits:
            entity = entities.get(hit["id"], {})
            if scientific.casefold() not in [v.casefold() for v in _values(entity, "P225")]:
                continue
            # A historical zoological combination can link to the current taxon.
            targets = _values(entity, "P12765")
            if len(targets) == 1:
                qid = targets[0]["id"]
                target = _entities(session, [qid]).get(qid, {})
                if _values(target, "P225"):
                    entity = target
            name = _values(entity, "P225")[0]
            return Taxon(name, entity.get("labels", {}).get("ko", {}).get("value", ""),
                         entity["id"], next(iter(_values(entity, "P373")), name))
    return Taxon(scientific, category=scientific)


def _files(session, query, basis):
    data = _api(session, COMMONS, action="query", generator="search", gsrnamespace=6,
                gsrsearch=query + " filetype:bitmap", gsrlimit=32, prop="imageinfo",
                iiprop="url|mime|extmetadata", iiurlwidth=500, iiextmetadatalanguage="en",
                iiextmetadatafilter="Artist|LicenseShortName|Categories")
    for page in sorted(data.get("query", {}).get("pages", []), key=lambda p: p["index"]):
        infos = page.get("imageinfo", [])
        if not infos:
            continue
        info = infos[0]
        if info.get("mime") not in {"image/jpeg", "image/webp", "image/png"} or not info.get("thumburl"):
            continue
        meta = {key: item["value"] for key, item in info.get("extmetadata", {}).items()}
        title = page["title"].removeprefix("File:")
        if NOT_PHOTO.search(title + "|" + meta.get("Categories", "")):
            continue
        yield Photo(page["pageid"], title, info["thumburl"], info["descriptionurl"],
                    plain(meta.get("Artist", "")) or "저자 정보는 원문 참조",
                    plain(meta.get("LicenseShortName", "")) or "라이선스는 원문 참조", basis)


@lru_cache(maxsize=32)
def search_photos(scientific):
    with requests.Session() as session:
        session.headers.update(HEADERS)
        try:
            taxon = resolve_taxon(session, scientific)
        except (requests.RequestException, RuntimeError) as error:
            # Commons can still be available when Wikidata is down.
            taxon = Taxon(scientific, category=scientific)
            lookup_error = error
        else:
            lookup_error = None
        queries = []
        if taxon.qid:
            queries.append((f"haswbstatement:P180={taxon.qid}", "종 태그"))
        category = taxon.category.replace('"', '')
        queries.append((f'incategory:"{category}"', "종 카테고리"))
        photos = {}
        for query, basis in queries:
            for photo in _files(session, query, basis):
                photos.setdefault(photo.page_id, photo)
                if len(photos) == 12:
                    return Gallery(taxon, tuple(photos.values()))
        if lookup_error and not photos:
            raise lookup_error
        return Gallery(taxon, tuple(photos.values()))


@lru_cache(maxsize=64)
def thumbnail(url):
    response = requests.get(url, headers=HEADERS, timeout=(5, 15))
    response.raise_for_status()
    return response.content


def reference_image(page_id):
    """Fetch the selected file at reconstruction resolution, independent of the gallery thumbnail."""
    from classifier import open_photo

    with requests.Session() as session:
        session.headers.update(HEADERS)
        data = _api(session, COMMONS, action="query", pageids=page_id,
                    prop="imageinfo", iiprop="url", iiurlwidth=1280)
        page = data["query"]["pages"][0]
        if not page.get("imageinfo"):
            raise ValueError("선택한 사진이 커먼스에서 삭제되었거나 이동했습니다.")
        info = page["imageinfo"][0]
        response = session.get(info.get("thumburl") or info["url"], timeout=(5, 30))
        response.raise_for_status()
        return open_photo(BytesIO(response.content), preserve_alpha=True)[0]
