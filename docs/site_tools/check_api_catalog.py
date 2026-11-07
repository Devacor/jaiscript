#!/usr/bin/env python3
"""Validate catalog schema, direct registration coverage, and generated artifacts.

This is a source-only check. Computed registrations (width families, vector
methods, aliases, typed signals) remain explicit catalog entries, reviewed with
their defining tables. Runtime examples and native regressions need the runner.
"""
import json
import pathlib
import re
import subprocess
import sys

SITE=pathlib.Path(__file__).resolve().parent.parent/"site"
PROJECT=SITE.parents[1]

def main():
	data=json.loads((SITE/'reference/api-catalog.json').read_text(encoding='utf-8'))
	entries=data['entries']
	keys=[(e['owner'],e['provider'],e['signature']) for e in entries]
	errors=[]
	if len(keys)!=len(set(keys)): errors.append('Duplicate signature entry')
	for e in entries:
		if not (PROJECT/e['source']).is_file(): errors.append('Missing source: '+e['source'])
		if not e.get('display') or not e.get('availability') or not e.get('effect'): errors.append('Incomplete entry: '+e['signature'])
	implementation=(PROJECT/'source/implementation/builtin_methods.cpp').read_text(encoding='utf-8')
	for owner,registry in [('array<T>','array'),('map<K,V>','map'),('string','string'),('weak_ptr<T>','weak_ptr'),('shared_ptr<T>','shared_ptr')]:
		start=implementation.index('out.'+registry+'_methods =')
		end=implementation.find('\n    out.',start+5)
		if end<0:end=len(implementation)
		registered=set(re.findall(r'symbolizer->intern\("([^"\n]+)"\)',implementation[start:end]))
		listed={e['name'] for e in entries if e['owner']==owner and e['form']=='method'}
		for missing in sorted(registered-listed):errors.append(owner+'.'+missing+' absent from catalog')
	for relative,owner,pattern in [
		('include/jaiscript/stdlib/reflection.hpp','reflect',r'add\("([^"\n]+)"'),
		('include/jaiscript/stdlib/strings.hpp','string',r'(?:sign|def)\("([^"\n]+)"'),
		('include/jaiscript/stdlib/json.hpp','global',r'add_(?:variadic_)?function\("([^"\n]+)"'),
		('source/implementation/stdlib/web_text.cpp','global',r'add_function_with_arity_and_full_types\("([^"\n]+)"'),
		('source/implementation/stdlib/crypto.cpp','crypto',r'\{"([a-z0-9_]+)", "crypto::'),
		('source/implementation/stdlib/crypto.cpp','crypto',r'define\("([a-z0-9_]+)"'),
		('source/implementation/stdlib/random.cpp','random',r'(?:\.method|add_method)\("([a-z][a-z_]*)"'),
		('tools/jaiscript/fetch.hpp','Download',r'\.method\("([^"\n]+)"'),
		('tools/jaiscript/sound.hpp','Sound',r'\.method\("([^"\n]+)"'),
		('tools/jaiscript/sound.hpp','global',r'add_function\("([^"\n]+)"'),
	]:
		registered=set(re.findall(pattern,(PROJECT/relative).read_text(encoding='utf-8')))
		listed={e['name'] for e in entries if e['owner']==owner}
		for missing in sorted(registered-listed):errors.append(relative+': '+missing+' absent from catalog')
	array=(PROJECT/'include/jaiscript/detail/array_namespace.hpp').read_text(encoding='utf-8')
	registered=set(re.findall(r'Transform\{"([^"]+)"',array))
	listed={e['name'] for e in entries if e['owner']=='array' and e['form']=='namespace'}
	for missing in sorted(registered-listed):errors.append('array::'+missing+' absent from catalog')
	math=(PROJECT/'include/jaiscript/detail/math_intrinsics.hpp').read_text(encoding='utf-8')
	lookup=math[math.index('inline math_fn math_intrinsic_from_name'):math.index('// Shared call-shape recognition')]
	public={name for name in re.findall(r'name == "([^"]+)"',lookup) if '$' not in name}
	listed={e['name'] for e in entries if e['owner']=='math'}
	for missing in sorted(public-listed):errors.append('math::'+missing+' absent from catalog')
	for extra in sorted(listed-public):errors.append('math::'+extra+' is not a public intrinsic name')
	if errors:
		print('\n'.join(errors))
		return 1
	result=subprocess.run([sys.executable,str(pathlib.Path(__file__).resolve().parent/'build_api_reference.py'),'--check'])
	if result.returncode:return result.returncode
	print(f'Catalog coverage: {len(entries)} signatures, {len(data["fields"])} fields/constants; direct registration checks pass')
	return 0

if __name__=='__main__':sys.exit(main())
