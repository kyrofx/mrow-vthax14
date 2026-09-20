"""Optional cloud model that reorders the heuristic's candidates. Standard library only.

Speaks either the OpenAI chat-completions protocol or the Anthropic Messages
protocol, so any compatible endpoint works. The model only reorders tracks the
heuristic already allowed and explains its picks; it never introduces a track,
and any failure (no network, timeout, refusal, malformed output) is reported to
the caller, which keeps the heuristic order. Offline is the normal case on a
WiFi-only device that moves between venues.
"""
import json
import os
import threading
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

PROVIDERS = ('openai', 'anthropic', 'openrouter')
DEFAULT_BASE_URLS = {'openai': 'https://api.openai.com/v1', 'anthropic': 'https://api.anthropic.com',
                     'openrouter': 'https://openrouter.ai/api/v1'}
DEFAULT_ENV_FILE = Path('~/.config/mrow/harness.env').expanduser()

SYSTEM_PROMPT = """You help a DJ choose the next song during a live set.

You receive the songs played so far in this set (most recent last), each with the
DJ's assessment of the crowd's reaction (good, mid, bad, or none), and a list of
candidate songs that already satisfy the DJ's tempo and key constraints, with a
heuristic score. Reorder the candidates: put the songs most likely to work next
first. Weigh the crowd's reactions most heavily, then tempo and key continuity,
genre flow, and artist variety. A bad reaction suggests changing direction.

Reply with only a JSON object, no other text:
{"picks": [{"id": "<candidate id>", "reason": "<why, at most 12 words>"}]}
Use only candidate ids from the list. Include at most the requested number of picks."""


class ModelError(Exception):
    """The model could not produce a usable ordering. The message is shown to the DJ."""


@dataclass(frozen=True)
class ModelConfig:
    provider: str
    model: str
    api_key: str
    base_url: str
    timeout: float = 10.0
    effort: str = ''  # Anthropic `output_config.effort`; empty omits it.
    candidates: int = 20

    def describe(self):
        return {'provider': self.provider, 'model': self.model, 'base_url': self.base_url}


def read_env_file(path):
    """KEY=VALUE lines; blank lines and # comments ignored; optional quotes stripped."""
    values = {}
    try:
        text = Path(path).read_text()
    except OSError:
        return values
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in '"\'':
            value = value[1:-1]
        values[key.strip()] = value
    return values


def load_config(environ=None, env_file=DEFAULT_ENV_FILE):
    """Model settings from the environment, falling back to the env file.

    Returns None when no model is configured, which disables the model and is
    not an error. Raises ValueError for a configuration that is present but
    unusable, so a typo fails loudly at startup instead of silently at a gig.
    """
    settings = read_env_file(env_file) if env_file else {}
    settings.update({k: v for k, v in (os.environ if environ is None else environ).items()
                     if k.startswith('MROW_MODEL')})
    provider = settings.get('MROW_MODEL_PROVIDER', '').strip().lower()
    model = settings.get('MROW_MODEL', '').strip()
    api_key = settings.get('MROW_MODEL_API_KEY', '').strip()
    if not (provider or model or api_key):
        return None
    if provider not in PROVIDERS:
        raise ValueError('MROW_MODEL_PROVIDER must be openai, anthropic or openrouter')
    if not model or not api_key:
        raise ValueError('MROW_MODEL and MROW_MODEL_API_KEY are both required')
    try:
        timeout = float(settings.get('MROW_MODEL_TIMEOUT', '10'))
        candidates = int(settings.get('MROW_MODEL_CANDIDATES', '20'))
    except ValueError:
        raise ValueError('MROW_MODEL_TIMEOUT and MROW_MODEL_CANDIDATES must be numbers') from None
    if not 1 <= timeout <= 60 or not 2 <= candidates <= 50:
        raise ValueError('MROW_MODEL_TIMEOUT must be 1–60 s and MROW_MODEL_CANDIDATES 2–50')
    effort = settings.get('MROW_MODEL_EFFORT', 'low' if provider == 'anthropic' else '').strip()
    base_url = settings.get('MROW_MODEL_BASE_URL', '').strip() or DEFAULT_BASE_URLS[provider]
    return ModelConfig(provider, model, api_key, base_url.rstrip('/'), timeout, effort, candidates)


class ModelClient:
    def __init__(self, config, urlopen=urllib.request.urlopen):
        self.config = config
        self.urlopen = urlopen
        self.cache = {}
        self.lock = threading.Lock()

    def rerank(self, played, candidates, count, cache_key=None):
        """Return [(track_id, reason)] in the model's order, at most `count` long.

        `played` and `candidates` are track dicts. Candidates are sent under
        short aliases, so track ids (which contain file paths) never leave the
        device; only song metadata does.
        """
        if cache_key is not None:
            with self.lock:
                if cache_key in self.cache:
                    return self.cache[cache_key]
        aliases = {f'c{i}': t['id'] for i, t in enumerate(candidates, 1)}
        payload = {
            'picks_wanted': count,
            'played': [describe(t) | {'crowd': t.get('rating') or 'none'} for t in played],
            'candidates': [describe(t) | {'id': alias, 'heuristic_score': round(t['score'], 3)}
                           for alias, t in zip(aliases, candidates)],
        }
        text = self.complete(json.dumps(payload, ensure_ascii=False))
        picks = parse_picks(text, aliases, count)
        if cache_key is not None:
            with self.lock:
                if len(self.cache) >= 64:
                    self.cache.pop(next(iter(self.cache)))
                self.cache[cache_key] = picks
        return picks

    def complete(self, user_message, system_prompt=SYSTEM_PROMPT):
        c = self.config
        if c.provider == 'anthropic':
            url = c.base_url + '/v1/messages'
            headers = {'x-api-key': c.api_key, 'anthropic-version': '2023-06-01'}
            body = {'model': c.model, 'max_tokens': 4000, 'system': system_prompt,
                    'messages': [{'role': 'user', 'content': user_message}]}
            if c.effort:
                body['output_config'] = {'effort': c.effort}
        else:
            url = c.base_url + '/chat/completions'
            headers = {'Authorization': 'Bearer ' + c.api_key}
            body = {'model': c.model, 'max_tokens': 4000, 'messages': [{'role': 'system', 'content': system_prompt},
                                                   {'role': 'user', 'content': user_message}]}
            if c.provider == 'openrouter':
                headers['X-Title'] = 'MROW Mixxx Agent'
        request = urllib.request.Request(url, json.dumps(body).encode(), method='POST',
                                         headers={'Content-Type': 'application/json', **headers})
        try:
            with self.urlopen(request, timeout=c.timeout) as response:
                reply = json.loads(response.read())
        except urllib.error.HTTPError as error:
            raise ModelError(f'Model returned HTTP {error.code}') from None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            reason = getattr(error, 'reason', error)
            raise ModelError(f'Model unreachable: {reason}') from None
        except ValueError:
            raise ModelError('Model returned invalid JSON') from None
        return response_text(c.provider, reply)


def describe(track):
    """The song metadata a model may see. No paths, drive labels, or ids."""
    result = {'title': track.get('title'), 'artist': track.get('artist') or None,
              'bpm': round(track['bpm'], 1) if track.get('bpm') else None,
              'key': track.get('camelot'), 'genres': track.get('genres') or None,
              'energy': track.get('energy')}
    return {k: v for k, v in result.items() if v is not None}


def response_text(provider, reply):
    if not isinstance(reply, dict):
        raise ModelError('Model returned an unexpected response')
    if provider == 'anthropic':
        if reply.get('stop_reason') == 'refusal':
            raise ModelError('Model declined the request')
        blocks = reply.get('content')
        if not isinstance(blocks, list):
            raise ModelError('Model returned an unexpected response')
        return ''.join(b.get('text', '') for b in blocks if isinstance(b, dict) and b.get('type') == 'text')
    try:
        return reply['choices'][0]['message']['content'] or ''
    except (KeyError, IndexError, TypeError):
        raise ModelError('Model returned an unexpected response') from None


def parse_picks(text, aliases, count):
    if not isinstance(text, str):
        raise ModelError('Model reply was not text')
    start, end = text.find('{'), text.rfind('}')
    if start < 0 or end <= start:
        raise ModelError('Model reply contained no JSON')
    try:
        picks = json.loads(text[start:end + 1]).get('picks')
    except (ValueError, AttributeError):
        raise ModelError('Model reply was not valid JSON') from None
    if not isinstance(picks, list):
        raise ModelError('Model reply had no picks')
    result, seen = [], set()
    for pick in picks:
        if not isinstance(pick, dict) or not isinstance(pick.get('id'), str) or pick['id'] not in aliases:
            continue  # Ignore invented or malformed entries rather than trusting them.
        track_id = aliases[pick['id']]
        if track_id in seen:
            continue
        seen.add(track_id)
        reason = pick.get('reason')
        result.append((track_id, reason.strip()[:200] if isinstance(reason, str) else ''))
        if len(result) == count:
            break
    if not result:
        raise ModelError('Model picked no valid candidates')
    return result
