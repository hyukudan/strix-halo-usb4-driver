#!/usr/bin/env python3
"""Full offline kernel/module and native HIP build; never install or execute it.

Default: validate inputs and print the intended commands. --execute creates a
new output directory and records real command results. Requires actual pristine
pinned Linux source and the ROCm development toolchain; no network or Actions.

Every driver input (patch series, provenance manifest, Kconfig fragment) is read
from --driver-repo, never from the directory holding this script. The manifest
provenance/software-validation.json is bound to the exact series by
series_sha256; regenerate it with tools/update_validation_manifest.py after any
patch change.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

PIN = '72d3fcf802c45d00b300f25b848a93c3a2bd7c7e'
ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'provenance/software-validation.json'
BASE_PATCH = '0001-usb4-current-source.patch'
PATCH_NAME = re.compile(r'^(\d{4})-[A-Za-z0-9][A-Za-z0-9._-]*\.patch$')
ENGINE_UAPI = 'include/strix/linux_direct_gpu_stream_uapi.h'
DRIVER_UAPI = 'include/uapi/linux/strix_gpu_stream.h'
ENGINE_FILES = (ENGINE_UAPI, 'src/linux_gpu_queue.cpp', 'src/gpu_queue_session.cpp')
OFFLINE_LOCALVERSION = '-strix-gpu-bosgame-offline-validation'

sys.path.insert(0, str(ROOT / 'tools'))
import check_kconfig  # noqa: E402


def env_clean():
    env = os.environ.copy()
    for key in ('GIT_DIR','GIT_WORK_TREE','GIT_INDEX_FILE','GIT_COMMON_DIR','MAKEFLAGS','MFLAGS'):
        env.pop(key,None)
    env['GIT_CEILING_DIRECTORIES'] = '/'
    return env


def query(argv, cwd=None):
    return subprocess.check_output(argv,cwd=cwd,env=env_clean(),text=True).strip()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def series_entries(repo, name):
    return [line for line in (repo/'patches'/name).read_text().splitlines()
            if line and not line.startswith('#')]


def series(repo):
    """The complete driver series of `repo`, whatever its length.

    Entries must be contiguous 0001..N, start with the captured base patch,
    be regular files, match patches/audit-series (series without 0001) and
    leave no unlisted top-level patch behind.
    """
    names=series_entries(repo,'series')
    if not names or names[0]!=BASE_PATCH:raise ValueError('patches/series must start with '+BASE_PATCH)
    result=[]
    for index,name in enumerate(names,1):
        match=PATCH_NAME.match(name)
        if Path(name).name!=name or not match:raise ValueError('Unsafe patch series entry: '+name)
        if int(match[1])!=index:raise ValueError(f'Patch series is not contiguous at {name} (expected {index:04d}-)')
        p=repo/'patches'/name
        if p.is_symlink() or not p.is_file():raise ValueError('Missing/symlink patch: '+name)
        result.append(p)
    if series_entries(repo,'audit-series')!=names[1:]:
        raise ValueError('patches/audit-series must equal patches/series without 0001')
    unlisted=sorted(p.name for p in (repo/'patches').glob('*.patch') if p.name not in names)
    if unlisted:raise ValueError('Unlisted patch in patches/: '+', '.join(unlisted))
    return result


def series_digest(patches):
    """sha256 over (name, sha256(content)) of every patch, in order."""
    h=hashlib.sha256()
    for p in patches:
        h.update(p.name.encode()+b'\0'+sha256(p).encode()+b'\n')
    return h.hexdigest()


def load_manifest(repo, patches):
    """The driver repo's manifest, checked against the series it was made for."""
    manifest=json.loads((repo/MANIFEST).read_text())
    if manifest.get('schema')!='usb4-software-validation-v2':
        raise ValueError(MANIFEST+' has an old schema; regenerate it with tools/update_validation_manifest.py')
    if manifest.get('series_sha256')!=series_digest(patches) or manifest.get('patch_count')!=len(patches):
        raise ValueError(MANIFEST+' was generated for a different patch series; '
                         'regenerate it with tools/update_validation_manifest.py')
    files=manifest.get('materialized_files')
    if not isinstance(files,dict) or DRIVER_UAPI not in files:
        raise ValueError(MANIFEST+' lacks the materialized UAPI header')
    engine=manifest.get('engine_files_sha256')
    if not isinstance(engine,dict) or set(engine)!=set(ENGINE_FILES):
        raise ValueError(MANIFEST+' must pin exactly '+', '.join(ENGINE_FILES))
    if engine[ENGINE_UAPI]!=files[DRIVER_UAPI]:
        raise ValueError('Engine/driver UAPI drift in '+MANIFEST+': the engine header must be '
                         'byte-identical to the materialized '+DRIVER_UAPI)
    return manifest


def touched_files(patches):
    """Paths that exist after applying the series (from the diff headers)."""
    files,old=set(),None
    for p in patches:
        for line in p.read_text(errors='replace').splitlines():
            if line.startswith('--- '):
                old=line[6:].split('\t')[0] if line.startswith('--- a/') else None
            elif line.startswith('+++ b/'):
                files.add(line[6:].split('\t')[0])
            elif line.startswith('+++ /dev/null') and old:
                files.discard(old)
    return files


def fingerprint(repo):
    names=subprocess.check_output(['git','-C',str(repo),'ls-files','-z'],env=env_clean()).split(b'\0')
    names=sorted({n.decode() for n in names if n})
    result={}
    for name in names:
        p=repo/name
        if p.is_symlink():continue
        if p.is_file() and (name=='CMakeLists.txt' or name.startswith(('include/','src/','lab/gpu_queue/','lab/gpu_visibility/','lab/direct_gpu_stream/'))):
            result[name]=hashlib.sha256(p.read_bytes()).hexdigest()
    return result


def verify_materialized_kernel(source, manifest):
    """Build only the exact materialized implementation recorded in the manifest."""
    for name,digest in manifest['materialized_files'].items():
        relative=Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Unsafe materialized path')
        path=source/relative
        if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise ValueError('Native source differs from delivered implementation: '+name)


def verify_engine(engine, manifest):
    for rel in ENGINE_FILES:
        path=engine/rel
        if path.is_symlink() or not path.is_file() or sha256(path)!=manifest['engine_files_sha256'][rel]:
            raise ValueError('Engine source differs from validated integration: '+rel+
                             (' (Engine/driver UAPI drift)' if rel==ENGINE_UAPI else ''))


def config_options(fragment_text, mode):
    """scripts/config arguments reproducing the fragment, plus the build identity."""
    opts=[]
    for name,value in check_kconfig.parse(fragment_text).items():
        if name=='CONFIG_LOCALVERSION':continue
        short=name[len('CONFIG_'):]
        if name=='CONFIG_USB4_GPU_STREAM' and mode=='off':value='n'
        opts+={'y':['-e',short],'m':['-m',short],'n':['-d',short]}.get(value,['--set-val',short,value])
    return opts+['--set-str','LOCALVERSION',OFFLINE_LOCALVERSION]


def main():
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--driver-repo',type=Path,default=ROOT)
    parser.add_argument('--engine-repo',required=True,type=Path)
    parser.add_argument('--kernel-checkout',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--hip-compiler',type=Path,default=Path('/opt/rocm/llvm/bin/clang++'))
    parser.add_argument('--jobs',type=int,default=2)
    parser.add_argument('--timeout-seconds',type=int,default=7200)
    parser.add_argument('--gpu-off-matrix',action='store_true')
    parser.add_argument('--execute',action='store_true')
    args=parser.parse_args()
    if not 1<=args.jobs<=32 or not 60<=args.timeout_seconds<=86400:raise ValueError('Invalid bounded build policy')
    driver,engine,kernel,output=(p.resolve() for p in (args.driver_repo,args.engine_repo,args.kernel_checkout,args.output))
    if output.exists():raise ValueError('Output must not exist')
    for p in (driver,engine,kernel):
        if p==output or p in output.parents or output in p.parents:
            raise ValueError('Use an output directory outside all input checkouts')
    if query(['git','-C',str(kernel),'rev-parse','HEAD'])!=PIN:
        raise ValueError('Kernel checkout is not the pinned revision')
    if query(['git','-C',str(kernel),'status','--porcelain=v1','--untracked-files=all']):
        raise ValueError('Kernel checkout must be pristine')
    patches=series(driver)
    manifest=load_manifest(driver,patches)
    fragment=(driver/'config/usb4-gpu.fragment').read_text()
    if not (engine/'lab/gpu_queue/CMakeLists.txt').is_file():raise ValueError('Queue integration is not applied')
    engine_before=fingerprint(engine)
    # Inputs must contain the final tested source, not a legacy partial UAPI.
    verify_engine(engine,manifest)
    tool=args.hip_compiler.resolve()
    modes=('on','off') if args.gpu_off_matrix else ('on',)
    source=output/'linux';commands=[]
    commands.append(['git','-C',str(kernel),'archive','--format=tar','--output='+str(output/'linux.tar'),PIN])
    for patch in patches:
        commands.extend([['git','-C',str(source),'apply','--check',str(patch)],['git','-C',str(source),'apply','--whitespace=error-all',str(patch)]])
    for mode in modes:
        dest=output/('kernel-gpu-'+mode)
        prefix=['make','-C',str(source),'O='+str(dest)]
        commands.append(prefix+['x86_64_defconfig'])
        commands.append([str(source/'scripts/config'),'--file',str(dest/'.config'),*config_options(fragment,mode)])
        commands.append(prefix+['olddefconfig'])
        commands.append(prefix+['-j'+str(args.jobs),'W=1','vmlinux','modules'])
    commands.extend([
        ['cmake','-S',str(engine/'lab/gpu_queue'),'-B',str(output/'hip'),'-G','Ninja','-DSTRIX_ENABLE_HIP_RUNTIME=ON','-DBUILD_TESTING=OFF','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_HIP_COMPILER='+str(tool),'-DCMAKE_HIP_ARCHITECTURES=gfx1151'],
        ['cmake','--build',str(output/'hip'),'--target','strix-gpu-queue-visibility','-j'+str(args.jobs)]])
    record=dict(schema='usb4-offline-native-build-v2',status='planned',hardware_executed=False,production_qualified=False,kernel=PIN,
                driver_repo=str(driver),patch_count=len(patches),series_sha256=series_digest(patches),commands=commands,
                engine_inputs_sha256=engine_before,patches_sha256={p.name:sha256(p) for p in patches},completed=[])
    if not args.execute:
        print(json.dumps(record,indent=2));return 0
    if not tool.is_file() or not os.access(tool,os.X_OK):raise ValueError('Actual ROCm clang++ is unavailable; nothing built')
    for name in ('cmake','make','ninja','git'):
        if not shutil.which(name):raise ValueError('Missing build tool: '+name)
    output.mkdir(parents=True);(output/'logs').mkdir();record_path=output/'build.json'
    def save():record_path.write_text(json.dumps(record,indent=2)+'\n')
    record['status']='running';save()
    try:
        for index,command in enumerate(commands):
            log=output/'logs'/f'{index:02d}.log'
            with log.open('wb') as stream:
                completed=subprocess.run(command,env=env_clean(),stdout=stream,stderr=subprocess.STDOUT,timeout=args.timeout_seconds)
            record['completed'].append(dict(index=index,returncode=completed.returncode,log=log.name));save()
            if completed.returncode:raise RuntimeError('Build command failed; see '+str(log))
            if index==0:
                source.mkdir()
                with tarfile.open(output/'linux.tar') as archive:archive.extractall(source,filter='data')
                (output/'linux.tar').unlink()
                subprocess.run(['git','-c','core.hooksPath=/dev/null','-C',str(source),'init','--quiet'],env=env_clean(),check=True)
            if index == 2 * len(patches):
                verify_materialized_kernel(source,manifest)
            if command[-1:] == ['olddefconfig']:
                cfg=Path(next(v[2:] for v in command if v.startswith('O=')))/'.config'
                on='gpu-on' in str(cfg.parent)
                problems=[p for p in check_kconfig.check(cfg.read_text(),fragment,gpu=on) if 'LOCALVERSION' not in p]
                if problems:raise RuntimeError('Kconfig dropped required settings: '+'; '.join(problems))
        if fingerprint(engine)!=engine_before:raise RuntimeError('Engine source changed during build')
        if {p.name:sha256(p) for p in patches}!=record['patches_sha256']:
            raise RuntimeError('Driver patches changed during build')
        record['status']='passed'
    except BaseException as error:
        record['status']='failed';record['error']=str(error);save();raise
    save();print('Native kernel/modules/HIP build passed. No resulting binary was installed or executed.');return 0


if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError,RuntimeError,subprocess.SubprocessError) as error:
        print('Native build refused/failed: '+str(error),file=sys.stderr);raise SystemExit(1)
