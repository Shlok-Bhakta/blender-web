# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the same Audaspace and sequencer operations on desktop and the web."""
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys
import time

import aud
import bpy
import numpy as np


def audio_summary(path):
    timings = []
    for _ in range(3):
        started = time.perf_counter()
        data = aud.Sound(str(path)).cache().data()
        timings.append(time.perf_counter() - started)
    assert data.ndim == 2 and data.shape[1] == 2 and len(data) >= 47000, (path, data.shape)
    indices = np.linspace(0, len(data) - 1, 257, dtype=np.int64)
    spectrum = np.abs(np.fft.rfft(data[:48000], axis=0))
    return dict(shape=list(data.shape), samples=data[indices].tolist(),
                rms=np.sqrt(np.mean(data.astype(np.float64) ** 2, axis=0)).tolist(),
                peakFrequency=np.rint(np.argmax(spectrum, axis=0) * 48000 / min(len(data), 48000)).astype(int).tolist(),
                pcmSha256=hashlib.sha256(data.astype('<f4').tobytes()).hexdigest(),
                decodeSeconds=statistics.median(timings), decodeRuns=timings)


def inspect_media(directory):
    result = dict(audio={}, movies={}, buildOptions={name:getattr(bpy.app.build_options, name)
                  for name in dir(bpy.app.build_options) if not name.startswith('_')
                  and isinstance(getattr(bpy.app.build_options, name), bool)})
    for name in ['tone.wav', 'tone.flac', 'tone.ogg', 'tone.mp3',
                 'reference.mkv', 'reference.mp4', 'reference.webm']:
        result['audio'][name] = audio_summary(directory / name)
    scene = bpy.context.scene
    scene.render.fps = 24
    scene.frame_start, scene.frame_end = 1, 24
    editor = scene.sequence_editor_create()
    for strip in list(editor.strips):
        editor.strips.remove(strip)
    for index, name in enumerate(['reference.mkv', 'reference.mp4', 'reference.webm']):
        strip = editor.strips.new_movie(name, str(directory / name), index + 1, 1)
        result['movies'][name] = dict(duration=strip.frame_final_duration,
            size=[strip.elements[0].orig_width, strip.elements[0].orig_height], fps=strip.fps)
        assert strip.frame_final_duration == 24
        assert result['movies'][name]['size'] == [128, 96]
        strip.mute = True
    editor.strips[0].mute = False
    editor.strips.new_sound('Stereo tone', str(directory / 'tone.wav'), 4, 1)
    return result


def compare_audio(actual, expected):
    for name, reference in expected['audio'].items():
        result = actual['audio'][name]
        assert result['shape'] == reference['shape'], (name, result['shape'], reference['shape'])
        error = float(np.max(np.abs(np.array(result['samples']) - reference['samples'])))
        assert error < 0.0003, (name, error)
        assert np.allclose(result['rms'], reference['rms'], rtol=0.001, atol=1e-5), name
        assert result['peakFrequency'] == reference['peakFrequency'] == [440, 660], name
        if name in {'tone.wav', 'tone.flac', 'reference.mkv', 'export.wav', 'export.flac'}:
            assert result['pcmSha256'] == reference['pcmSha256'], name
        result['maxSampleError'] = error
    assert actual['movies'] == expected['movies']


def export_audio(directory):
    source = aud.Sound(str(directory / 'tone.wav'))
    outputs = {}
    for extension, container, codec in [
        ('wav', aud.CONTAINER_WAV, aud.CODEC_PCM),
        ('flac', aud.CONTAINER_FLAC, aud.CODEC_FLAC),
        ('ogg', aud.CONTAINER_OGG, aud.CODEC_VORBIS),
        ('mp3', aud.CONTAINER_MP3, aud.CODEC_MP3),
    ]:
        target = directory / f'export.{extension}'
        source.write(str(target), 48000, 2, aud.FORMAT_S16, container, codec, 192000, 1024)
        assert target.stat().st_size > 1000
        outputs[target.name] = dict(bytes=target.stat().st_size, **audio_summary(target))
    return outputs


def stretch_audio(directory):
    source = aud.Sound(str(directory / 'tone.wav'))
    outputs = {}
    for name, ratio, pitch, hz in [
        ('slow', 2.0, 1.0, [440, 660]),
        ('fast', .5, 1.0, [440, 660]),
        ('pitch', 1.0, 1.5, [660, 990]),
    ]:
        started = time.perf_counter()
        sound = source.timeStretchPitchScale(ratio, pitch).cache()
        data = sound.data()
        frames = len(data)
        assert data.shape[1] == 2 and abs(frames / 48000 - ratio) < .15, (name, data.shape)
        middle = data[frames // 4:3 * frames // 4]
        frequencies = (np.argmax(np.abs(np.fft.rfft(middle, axis=0)), axis=0) * 48000 / len(middle)).tolist()
        assert np.allclose(frequencies, hz, atol=3), (name, frequencies)
        target = directory / f'stretch-{name}.wav'
        sound.write(str(target), 48000, 2, aud.FORMAT_S16, aud.CONTAINER_WAV, aud.CODEC_PCM, 192000, 1024)
        outputs[name] = dict(frames=frames, peakHz=frequencies, timeRatio=ratio,
                             pitchScale=pitch, seconds=time.perf_counter()-started)
    return outputs


def render_media(directory):
    scene = bpy.context.scene
    # A CPU renderer lets the same sequencer pipeline run on software WebGPU.
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 1
    scene.cycles.use_denoising = False
    scene.render.resolution_x, scene.render.resolution_y = 128, 96
    scene.render.resolution_percentage = 100
    scene.render.threads_mode = 'FIXED'
    scene.render.threads = 8
    scene.render.use_sequencer = True
    scene.render.use_compositing = False
    scene.view_settings.view_transform = 'Standard'
    scene.view_settings.look = 'None'
    scene.render.image_settings.media_type = 'IMAGE'
    scene.render.image_settings.file_format = 'PNG'
    frames = {}
    for frame in [1, 12, 24, 6]:
        scene.frame_set(frame)
        scene.render.filepath = str(directory / f'frame-{frame}.png')
        assert bpy.ops.render.render(write_still=True) == {'FINISHED'}
        image = bpy.data.images.load(scene.render.filepath, check_existing=False)
        data = np.empty(len(image.pixels), dtype=np.float32)
        image.pixels.foreach_get(data)
        frames[str(frame)] = data.reshape(96, 128, 4)[48, 64].tolist()
        bpy.data.images.remove(image)
    exports = {}
    for extension, container, codec, audio in [
        ('mp4', 'MPEG4', 'H264', 'AAC'), ('webm', 'WEBM', 'WEBM', 'OPUS'),
        ('av1.mkv', 'MKV', 'AV1', 'AAC'), ('hevc.mkv', 'MKV', 'H265', 'AAC'),
    ]:
        # Upstream libaom tiling needs more than the tiny 128x96 probe.
        scene.render.resolution_x, scene.render.resolution_y = (512, 512) if codec == 'AV1' else (128, 96)
        scene.render.image_settings.media_type = 'VIDEO'
        scene.render.image_settings.file_format = 'FFMPEG'
        scene.render.ffmpeg.format = container
        scene.render.ffmpeg.codec = codec
        scene.render.ffmpeg.audio_codec = audio
        scene.render.ffmpeg.audio_mixrate = 48000
        scene.render.ffmpeg.audio_bitrate = 192
        scene.render.filepath = str(directory / f'export.{extension}')
        runs = []
        for _ in range(3):
            started = time.perf_counter()
            assert bpy.ops.render.render(animation=True) == {'FINISHED'}
            runs.append(time.perf_counter() - started)
        candidates = list(directory.glob(f'export*{extension}'))
        assert len(candidates) == 1, candidates
        target = candidates[0]
        exports[target.name] = dict(bytes=target.stat().st_size, seconds=statistics.median(runs), runs=runs,
                                    audio=audio_summary(target))
        # Reimport each exported movie through Blender's original decoder.
        editor = scene.sequence_editor
        visible = [(strip, strip.mute) for strip in editor.strips if strip.type == 'MOVIE']
        for strip, _ in visible:
            strip.mute = True
        imported = editor.strips.new_movie('Roundtrip ' + codec, str(target), 7, 1)
        # AAC padding can extend the container estimate by one timeline frame.
        assert 24 <= imported.frame_final_duration <= 25, (codec, imported.frame_final_duration)
        exports[target.name]['reimportDuration'] = imported.frame_final_duration
        scene.render.image_settings.media_type = 'IMAGE'
        scene.render.image_settings.file_format = 'PNG'
        scene.frame_set(12)
        scene.render.filepath = str(directory / f'roundtrip-{codec}.png')
        assert bpy.ops.render.render(write_still=True) == {'FINISHED'}
        image = bpy.data.images.load(scene.render.filepath, check_existing=False)
        pixels = np.empty(len(image.pixels), dtype=np.float32)
        image.pixels.foreach_get(pixels)
        width, height = scene.render.resolution_x, scene.render.resolution_y
        pixel = pixels.reshape(height, width, 4)[height // 2, width // 2].tolist()
        assert np.allclose(pixel, frames['12'], atol=.03), (codec, pixel, frames['12'])
        exports[target.name]['roundtripPixel'] = pixel
        bpy.data.images.remove(image)
        editor.strips.remove(imported)
        for strip, muted in visible:
            strip.mute = muted

    scene.render.image_settings.media_type = 'IMAGE'
    scene.render.image_settings.file_format = 'PNG'
    scene.render.resolution_x, scene.render.resolution_y = 128, 96
    return dict(seekFrames=frames, videos=exports)


def browser_check(directory, render=True):
    directory = Path(directory)
    try:
        expected = json.loads((directory / 'reference.json').read_text())
        actual = inspect_media(directory)
        compare_audio(actual, expected)
        actual['exports'] = export_audio(directory)
        compare_audio({'audio':actual['exports'], 'movies':{}},
                      {'audio':expected['exports'], 'movies':{}})
        actual['stretch'] = stretch_audio(directory)
        for name, reference in expected['stretch'].items():
            assert abs(actual['stretch'][name]['frames'] - reference['frames']) < 256
            assert np.allclose(actual['stretch'][name]['peakHz'], reference['peakHz'], atol=3)
        if render:
            actual['render'] = render_media(directory)
            for frame, pixel in actual['render']['seekFrames'].items():
                assert np.allclose(pixel, expected['render']['seekFrames'][frame], atol=1e-5), (frame, pixel)
        # Keep the original sequencer populated for interactive playback tests.
        scene = bpy.context.scene
        scene.frame_set(1)
        scene.sync_mode = 'AUDIO_SYNC'
        scene.render.image_settings.media_type = 'IMAGE'
        scene.render.image_settings.file_format = 'PNG'
        bpy.ops.wm.save_as_mainfile(filepath=str(directory / 'media.blend'))
        actual['sourceSha256'] = hashlib.sha256((directory / 'check.py').read_bytes()).hexdigest()
        assert actual['sourceSha256'] == expected['sourceSha256']
        (directory / 'results.json').write_text(json.dumps(actual, indent=2) + '\n')
        os.write(1, ('MEDIA_DONE:' + json.dumps(actual) + '\n').encode())
    except BaseException:
        import traceback
        os.write(1, ('MEDIA_FAILED:' + traceback.format_exc() + '\n').encode())
        raise
    finally:
        os.write(1, b'MEDIA_FINISHED\n')


if __name__ == '__main__' and '--' in sys.argv:
    directory = Path(sys.argv[sys.argv.index('--') + 1])
    result = inspect_media(directory)
    result['exports'] = export_audio(directory)
    result['stretch'] = stretch_audio(directory)
    result['render'] = render_media(directory)
    result['sourceSha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (directory / 'reference.json').write_text(json.dumps(result, indent=2) + '\n')
