#!/usr/bin/env python3
"""Full offline kernel/module and native HIP build; never install or execute it.

Default: validate inputs and print the intended commands. --execute creates a
new output directory and records real command results. Requires actual pristine
pinned Linux source and the ROCm development toolchain; no network or Actions.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

PIN = '72d3fcf802c45d00b300f25b848a93c3a2bd7c7e'
ROOT = Path(__file__).resolve().parents[1]


def env_clean():
    env = os.environ.copy()
    for key in ('GIT_DIR','GIT_WORK_TREE','GIT_INDEX_FILE','GIT_COMMON_DIR','MAKEFLAGS','MFLAGS'):
        env.pop(key,None)
    env['GIT_CEILING_DIRECTORIES'] = '/'
    return env


def query(argv, cwd=None):
    return subprocess.check_output(argv,cwd=cwd,env=env_clean(),text=True).strip()


def series(repo):
    result=[]
    for name in (repo/'patches/series').read_text().splitlines():
        if not name or name.startswith('#'):continue
        if Path(name).name != name or not name.endswith('.patch'):
            raise ValueError('Unsafe patch series entry')
        p=repo/'patches'/name
        if p.is_symlink() or not p.is_file():raise ValueError('Missing/symlink patch')
        result.append(p)
    if len(result)!=9 or not result[-1].name.startswith('0009-'):
        raise ValueError('Expected the complete nine-patch driver series')
    return result


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



def verify_materialized_kernel(source):
    """Build only the exact materialized implementation tested in the published integration.

    Files retained only as partial hook contexts are deliberately not in this
    inventory; those come from the actual pristine pinned Git tree + patches.
    """
    files=json.loads((ROOT/'provenance/software-validation.json').read_text())['materialized_files']
    for name,digest in files.items():
        relative=Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Unsafe materialized path')
        path=source/relative
        if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise ValueError('Native source differs from delivered implementation: '+name)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
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
    if not (engine/'lab/gpu_queue/CMakeLists.txt').is_file():raise ValueError('Queue integration is not applied')
    engine_before=fingerprint(engine)
    # Inputs must contain the final tested source, not a legacy partial UAPI.
    for rel in ('include/strix/linux_direct_gpu_stream_uapi.h','src/linux_gpu_queue.cpp','src/gpu_queue_session.cpp'):
        expected=json.loads((ROOT/'provenance/software-validation.json').read_text())['engine_files_sha256'][rel]
        if hashlib.sha256((engine/rel).read_bytes()).hexdigest()!=expected:raise ValueError('Engine source differs from validated integration: '+rel)
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
        opts=['--file',str(dest/'.config'),'-m','USB4','-m','USB4_NET','-m','USB4_STREAM','-e','CONFIGFS_FS','-e','DMA_SHARED_BUFFER','-e','SYNC_FILE']
        opts+=['-e' if mode=='on' else '-d','USB4_GPU_STREAM','--set-str','LOCALVERSION','-strix-usb4-offline']
        commands.append([str(source/'scripts/config'),*opts])
        commands.append(prefix+['olddefconfig'])
        commands.append(prefix+['-j'+str(args.jobs),'W=1','vmlinux','modules'])
    commands.extend([
        ['cmake','-S',str(engine/'lab/gpu_queue'),'-B',str(output/'hip'),'-G','Ninja','-DSTRIX_ENABLE_HIP_RUNTIME=ON','-DBUILD_TESTING=OFF','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_HIP_COMPILER='+str(tool),'-DCMAKE_HIP_ARCHITECTURES=gfx1151'],
        ['cmake','--build',str(output/'hip'),'--target','strix-gpu-queue-visibility','-j'+str(args.jobs)]])
    record=dict(schema='usb4-offline-native-build-v1',status='planned',hardware_executed=False,production_qualified=False,kernel=PIN,commands=commands,
                engine_inputs_sha256=engine_before,patches_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in patches},completed=[])
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
                verify_materialized_kernel(source)
            if command[-1:] == ['olddefconfig']:
                cfg=Path(next(v[2:] for v in command if v.startswith('O=')))/'.config'
                text=cfg.read_text()
                for setting in ('CONFIG_USB4=m','CONFIG_USB4_NET=m','CONFIG_USB4_STREAM=m'):
                    if setting not in text.splitlines():raise RuntimeError('Kconfig disabled required consumer: '+setting)
                on='gpu-on' in str(cfg.parent)
                if on and 'CONFIG_USB4_GPU_STREAM=y' not in text.splitlines():raise RuntimeError('GPU stream option did not remain enabled')
                if not on and 'CONFIG_USB4_GPU_STREAM=y' in text.splitlines():raise RuntimeError('GPU-off matrix was not disabled')
        if fingerprint(engine)!=engine_before:raise RuntimeError('Engine source changed during build')
        if {p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in patches}!=record['patches_sha256']:
            raise RuntimeError('Driver patches changed during build')
        record['status']='passed'
    except BaseException as error:
        record['status']='failed';record['error']=str(error);save();raise
    save();print('Native kernel/modules/HIP build passed. No resulting binary was installed or executed.');return 0


if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError,RuntimeError,subprocess.SubprocessError) as error:
        print('Native build refused/failed: '+str(error),file=sys.stderr);raise SystemExit(1)
