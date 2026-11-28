"""Pretrained CLIP cues for reference view/posture selection, not 3D measurements."""
import gc
import numpy as np

PROMPTS = {
    'medium': {
        'photo': 'a real photograph of a living animal',
        'art': 'a drawing, painting, print or illustration of an animal',
        'object': 'a sculpture, toy, stuffed animal or taxidermy specimen',
    },
    'view': {
        'front': 'a photograph of an animal viewed from the front, facing the camera',
        'side': 'a photograph of an animal in side profile, viewed from the side',
        'rear': 'a photograph of an animal viewed from behind, showing its back and rump',
        'top': 'a photograph of an animal viewed from above, looking down at its back',
        'oblique': 'a photograph of an animal viewed at a three quarter angle',
    },
    'pose': {
        'lying': 'a photograph of an animal resting on its belly, chest on the ground, front paws together in front of its body',
        'sitting': 'a photograph of an animal sitting tall and upright on its haunches, with straight vertical front legs and chest raised high above the ground',
        'standing': 'a photograph of an animal standing on all four legs',
        'curled': 'a photograph of an animal curled up sleeping in a ball',
        'moving': 'a photograph of an animal walking or running',
        'supine': 'a photograph of an animal lying on its back or rolling on its side, exposing its belly',
    },
    'framing': {
        'whole': 'a photograph showing the whole animal including its head, body and feet',
        'head': 'a close up portrait of an animal face and eyes, with the body outside the picture',
        'detail': 'a close up photograph of one small animal body part, without the whole animal',
        'occluded': 'a photograph of an animal partially hidden behind another object',
        'multiple': 'a photograph of several animals together',
    },
    'limb_visibility': {
        'clear': 'a photograph of an animal with visible individual legs and paws, distinct from its body',
        'overlap': 'a photograph of an animal with overlapping legs, paws tucked against its body',
        'hidden': 'a photograph of an animal whose legs and paws are hidden from view',
    },
}

DOG_BREEDS = ['Welsh corgi', 'German shepherd', 'golden retriever', 'Labrador retriever',
              'Siberian husky', 'Alaskan malamute', 'Bernese mountain dog', 'border collie',
              'rough collie', 'Shetland sheepdog', 'Shiba inu', 'Akita', 'beagle', 'dachshund',
              'basset hound', 'greyhound', 'whippet', 'Chihuahua', 'Pomeranian', 'poodle',
              'Maltese', 'Shih Tzu', 'Yorkshire terrier', 'Jack Russell terrier', 'West Highland terrier',
              'schnauzer', 'French bulldog', 'English bulldog', 'pug', 'boxer', 'Rottweiler',
              'Doberman', 'Great Dane', 'Samoyed', 'Chow Chow', 'Dalmatian', 'mixed breed dog']


class PhotoContextEncoder:
    def __init__(self, progress=print, device='cpu', animal_name='animal'):
        import torch
        from transformers import CLIPModel, CLIPProcessor
        from model_cache import cached_asset
        model_id = ('openai/clip-vit-base-patch32', 'model', '3d74acf9a28c67741b2f4f2ea7635f0aaf6f0268')
        files = ('config.json', 'preprocessor_config.json', 'tokenizer.json', 'tokenizer_config.json',
                 'special_tokens_map.json', 'vocab.json', 'merges.txt', 'pytorch_model.bin')
        directory = None
        for filename in files:
            directory = cached_asset(model_id, filename, progress).parent
        progress('사전학습 시각 모델로 촬영 방향과 자세를 비교하는 중')
        torch.set_num_threads(min(6, torch.get_num_threads()))
        self.processor = CLIPProcessor.from_pretrained(directory, local_files_only=True)
        model = CLIPModel.from_pretrained(directory, local_files_only=True).to(device).eval()
        self.device, self.visual = device, model
        self.prompts = dict(PROMPTS)
        if 'dog' in animal_name.lower() or 'canis' in animal_name.lower():
            self.prompts['breed'] = {name: f'a photograph of a {name} dog' for name in DOG_BREEDS}
        self.embeddings = {}
        with torch.inference_mode():
            for group, choices in self.prompts.items():
                self.embeddings[group] = torch.nn.functional.normalize(
                    model.get_text_features(**self.processor(text=[p.replace('animal', animal_name) for p in choices.values()], return_tensors='pt',
                                                            padding=True).to(device)), dim=-1)
        self.scale = min(50., float(model.logit_scale.detach().exp()))

    def analyse(self, image, context_image=None):
        import torch
        # Composite cutouts on white instead of turning transparent regions black.
        if image.mode == 'RGBA':
            from PIL import Image
            rgb = Image.new('RGB', image.size, 'white')
            rgb.paste(image, mask=image.getchannel('A'))
            image = rgb
        with torch.inference_mode():
            feature = torch.nn.functional.normalize(
                self.visual.get_image_features(**self.processor(images=image.convert('RGB'), return_tensors='pt').to(self.device)), dim=-1)
            scene_feature = feature if context_image is None else torch.nn.functional.normalize(
                self.visual.get_image_features(**self.processor(images=context_image.convert('RGB'), return_tensors='pt').to(self.device)), dim=-1)
            result = {'model': 'OpenAI CLIP ViT-B/32', 'method': 'zero-shot visual cues; scores are not calibrated probabilities'}
            for group, choices in self.prompts.items():
                # Matting can make a real photo resemble a toy, or hide other
                # animals. Judge medium/framing from the complete reference.
                selected_feature = scene_feature if group in {'medium', 'framing'} else feature
                scores = (self.scale * selected_feature @ self.embeddings[group].T).softmax(-1)[0].cpu().numpy()
                order = np.argsort(scores)[::-1]
                labels = list(choices)
                confident = scores[order[0]] >= .35 and scores[order[0]]-scores[order[1]] >= .1
                result[group] = {'label': labels[order[0]] if confident else 'uncertain',
                                 'bestCandidate': labels[order[0]], 'confident': bool(confident),
                                 'score': float(scores[order[0]]),
                                 'margin': float(scores[order[0]]-scores[order[1]]),
                                 'scores': dict(zip(labels, map(float, scores)))}
        return result

    def close(self):
        self.visual = None
        self.embeddings.clear()
        gc.collect()


def reference_view(reference):
    """Selection cue, with provenance kept separate from CLIP's confidence."""
    context = reference.get('photoContext', {})
    if context.get('framing', {}).get('label') in {'head', 'detail', 'multiple'}:
        return 'uncertain'
    hint = reference.get('view_hint')
    if hint in {'side', 'rear', 'top'}:
        return hint
    return context.get('view', {}).get('label', 'uncertain')


def choose_reference_indices(measured, limit=4):
    """Balance colour similarity with useful view coverage; avoid head-only crops."""
    if not measured:
        return []
    def score(index):
        distance, reference, _, _ = measured[index]
        context = reference.get('photoContext', {})
        framing = context.get('framing', {})
        penalty = .5 if framing.get('label') in {'head', 'detail', 'multiple'} and framing.get('margin', 0) > .1 else 0
        return distance + penalty
    eligible = [i for i in range(len(measured))
                if measured[i][1].get('photoContext', {}).get('framing', {}).get('label')
                not in {'detail', 'multiple'}]
    ranked = sorted(eligible, key=score)
    selected = []
    # A reference from a different animal is evidence of body structure/material,
    # never a calibrated camera view of the original individual.
    for wanted in ('side', 'rear', 'top'):
        matches = [i for i in ranked if reference_view(measured[i][1]) == wanted]
        if matches and len(selected) < limit:
            selected.append(matches[0])
    selected += [i for i in ranked if i not in selected][:limit-len(selected)]
    return selected
