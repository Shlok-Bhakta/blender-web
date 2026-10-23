# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the same original tracking and camera-solve operators in both builds."""
import hashlib
import json
import math
from pathlib import Path
import time


WIDTH, HEIGHT, FRAMES = 256, 192, 12


def camera_center(frame):
    return (.12 * (frame - 1), .025 * math.sin(frame / 3), .015 * math.cos(frame / 4))


def fixtures(directory):
    import bpy
    output = Path(directory)
    output.mkdir(parents=True, exist_ok=True)
    # A deterministic textured patch, translated by two/one pixels per frame.
    for frame in range(1, FRAMES + 1):
        pixels = []
        dx, dy = 2 * (frame - 1), frame - 1
        for y in range(HEIGHT):
            for x in range(WIDTH):
                u, v = x - dx, y - dy
                value = ((u * 73856093 ^ v * 19349663) & 255) / 255 if 60 <= u < 100 and 60 <= v < 100 else .12
                pixels.extend((value, value, value, 1))
        image = bpy.data.images.new('Tracking fixture', WIDTH, HEIGHT)
        image.pixels.foreach_set(pixels)
        image.filepath_raw = str(output / f'frame{frame:03}.png')
        image.file_format = 'PNG'
        image.save()
        bpy.data.images.remove(image)


def clip_context(clip):
    import bpy
    area = max((a for a in bpy.context.screen.areas if a.type != 'CONSOLE'),
               key=lambda a: a.width * a.height)
    area.type = 'CLIP_EDITOR'
    area.spaces.active.clip = clip
    area.spaces.active.mode = 'TRACKING'
    region = next(region for region in area.regions if region.type == 'WINDOW')
    return bpy.context.temp_override(area=area, region=region)


def check(directory):
    import bpy
    from mathutils import Matrix, Vector
    assert bpy.app.build_options.libmv
    directory = Path(directory)
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = 1, FRAMES
    scene.frame_set(1)
    clip = bpy.data.movieclips.load(str(directory / 'frame001.png'), check_existing=False)
    clip.name = 'Tracking pixel motion'
    assert list(clip.size) == [WIDTH, HEIGHT]
    clip.tracking.settings.speed = 'FASTEST'
    track = clip.tracking.tracks.new(name='Known pixel motion', frame=1)
    track.markers[0].co = (80 / WIDTH, 80 / HEIGHT)
    track.select = True
    track.motion_model = 'Loc'
    started = time.perf_counter()
    with clip_context(clip):
        assert bpy.ops.clip.track_markers('EXEC_DEFAULT', sequence=True) == {'FINISHED'}
    tracking_seconds = time.perf_counter() - started
    markers, maximum = [], 0
    for frame in range(1, FRAMES + 1):
        marker = track.markers.find_frame(frame, exact=True)
        assert marker and not marker.mute, ('Missing tracked marker', frame)
        expected = [(80 + 2 * (frame - 1)) / WIDTH, (80 + frame - 1) / HEIGHT]
        error = math.hypot((marker.co[0] - expected[0]) * WIDTH, (marker.co[1] - expected[1]) * HEIGHT)
        maximum = max(maximum, error)
        markers.append(dict(frame=frame, co=list(marker.co), expected=expected, errorPixels=error))
    assert maximum < .15, maximum

    # A second clip has exact observations of non-coplanar 3D points. Ceres
    # solves through Blender's original reconstruction operator and libmv.
    solve_clip = bpy.data.movieclips.load(str(directory / 'frame001.png'), check_existing=False)
    solve_clip.name = 'Tracking camera solve'
    settings = solve_clip.tracking.settings
    settings.use_keyframe_selection = False
    settings.refine_intrinsics_focal_length = False
    settings.refine_intrinsics_principal_point = False
    settings.refine_intrinsics_radial_distortion = False
    camera = solve_clip.tracking.camera
    camera.focal_length_pixels = 220
    camera.principal_point = (0, 0)
    obj = solve_clip.tracking.objects[0]
    obj.keyframe_a, obj.keyframe_b = 1, FRAMES
    for index in range(32):
        point = Vector((((index * 37) % 23 - 11) / 8, ((index * 19) % 17 - 8) / 7,
                        4 + (index * 13 % 19) / 5))
        t = obj.tracks.new(name=f'Point {index:02}', frame=1)
        for frame in range(1, FRAMES + 1):
            center = Vector(camera_center(frame))
            rotation = Matrix.Rotation(.008 * (frame - 1), 3, 'Y')
            local = rotation @ (point - center)
            marker = t.markers[0] if frame == 1 else t.markers.insert_frame(frame)
            marker.co = (.5 + 220 * local.x / local.z / WIDTH,
                         .5 + 220 * local.y / local.z / HEIGHT)
    scene.frame_set(1)
    started = time.perf_counter()
    with clip_context(solve_clip):
        assert bpy.ops.clip.solve_camera('EXEC_DEFAULT') == {'FINISHED'}
    solve_seconds = time.perf_counter() - started
    reconstruction = obj.reconstruction
    assert reconstruction.is_valid
    assert len(reconstruction.cameras) == FRAMES
    assert reconstruction.average_error < .05, reconstruction.average_error
    bundles = [t for t in obj.tracks if t.has_bundle]
    assert len(bundles) == 32
    assert all(t.average_error < .1 for t in bundles)
    centers = [c.matrix.translation for c in reconstruction.cameras]
    baseline = (centers[-1] - centers[0]).length
    assert baseline > 1e-5
    distance_ratios = [(center - centers[0]).length / baseline for center in centers]
    expected_centers = [Vector(camera_center(frame)) for frame in range(1, FRAMES + 1)]
    expected_baseline = (expected_centers[-1] - expected_centers[0]).length
    expected_ratios = [(center - expected_centers[0]).length / expected_baseline for center in expected_centers]
    trajectory_error = max(abs(a - b) for a, b in zip(distance_ratios, expected_ratios))
    assert trajectory_error < .01, trajectory_error
    result = dict(blenderVersion=bpy.app.version_string, libmv=bpy.app.build_options.libmv,
        markers=markers, maximumTrackErrorPixels=maximum, trackingSeconds=tracking_seconds,
        solve=dict(valid=reconstruction.is_valid, cameras=len(reconstruction.cameras),
            bundles=len(bundles), averageErrorPixels=reconstruction.average_error,
            maximumTrackErrorPixels=max(t.average_error for t in bundles), seconds=solve_seconds,
            cameraDistanceRatios=distance_ratios, maximumTrajectoryRatioError=trajectory_error),
        sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        fixtureHashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(directory.glob('frame*.png'))})
    (directory / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def saved_check():
    import bpy
    clip = bpy.data.movieclips['Tracking pixel motion']
    track = clip.tracking.tracks['Known pixel motion']
    markers = [list(track.markers.find_frame(frame, exact=True).co)
               for frame in range(1, FRAMES + 1)]
    solved = bpy.data.movieclips['Tracking camera solve'].tracking.objects[0]
    assert solved.reconstruction.is_valid and len(solved.reconstruction.cameras) == FRAMES
    assert sum(t.has_bundle for t in solved.tracks) == 32
    assert solved.reconstruction.average_error < .05
    return dict(markers=markers, cameras=len(solved.reconstruction.cameras),
                bundles=sum(t.has_bundle for t in solved.tracks),
                averageErrorPixels=solved.reconstruction.average_error)


def browser_check(directory):
    try:
        return check(directory)
    except BaseException:
        import os
        import traceback
        os.write(1, ('TRACKING_FAILED:' + traceback.format_exc() + '\n').encode())
        raise


if __name__ == '__main__':
    import sys
    args = sys.argv[sys.argv.index('--') + 1:]
    if args[0] == 'fixtures':
        fixtures(args[1])
    else:
        print('TRACKING_COMPLETE:' + json.dumps(check(args[1])))
