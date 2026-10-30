# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise original OSL Python query APIs on the same bytecode in both builds."""
import concurrent.futures
import json
from pathlib import Path


def run(directory, fixture='/fixtures/web_query.oso'):
    import OpenImageIO as oiio
    import oslquery

    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    assert oslquery.VERSION_STRING == '1.15.3.0'
    assert hasattr(oslquery, '__path__')
    bytecode = Path(fixture).read_text()
    filename = directory / 'shader-雪.oso'
    filename.write_text(bytecode)

    def parameter(p):
        assert isinstance(p.type, oiio.TypeDesc)
        return dict(name=p.name, type=str(p.type), output=p.isoutput,
                    variableArray=p.varlenarray, structure=p.isstruct,
                    closure=p.isclosure, value=p.value,
                    metadata=[parameter(m) for m in p.metadata])

    def inspect(query):
        assert not query.geterror()
        assert query.nparams == len(query) == 9
        parameters = [parameter(p) for p in query]
        assert parameters == [parameter(p) for p in query.parameters]
        assert parameter(query[0]) == parameter(query['gain']) == parameters[0]
        assert query['gain'].value == 1.25 and query['steps'].value == 7
        assert query['label'].value == 'browser'
        assert query['tint'].value == (.25, .5, .75)
        assert query['axis'].value == (1., 2., 3.)
        assert query['weights'].value == (.125, .25, .625)
        assert query['basis'].value == (1., 0., 0., 0., 0., 1., 0., 0.,
                                        0., 0., 1., 0., 0., 0., 0., 1.)
        assert query['result'].isoutput and query['energy'].isoutput
        assert query['gain'].metadata[0].name == 'label'
        assert query['gain'].metadata[0].value == 'Gain'
        for key, exception in ((99, IndexError), ('absent', KeyError)):
            try:
                query[key]
            except exception:
                pass
            else:
                raise AssertionError(f'Invalid parameter lookup accepted: {key}')
        return dict(name=query.shadername(), type=query.shadertype(),
                    parameters=parameters, metadata=[parameter(p) for p in query.metadata])

    expected = inspect(oslquery.OSLQuery(str(filename)))
    memory = oslquery.OSLQuery()
    assert memory.open_bytecode(bytecode)
    assert inspect(memory) == expected
    searched = oslquery.OSLQuery()
    assert searched.open(filename.stem, str(directory))
    assert inspect(searched) == expected
    invalid = oslquery.OSLQuery()
    assert not invalid.open_bytecode('not valid OSL bytecode\n')
    # Original OSL rejects this header without emitting a diagnostic.
    assert invalid.geterror() == ''
    assert not invalid.open('missing-query-fixture', str(directory))
    diagnostic = invalid.geterror(False)
    assert diagnostic and invalid.geterror() == diagnostic
    assert invalid.geterror() == ''
    assert invalid.open_bytecode(bytecode) and inspect(invalid) == expected

    def inspect_thread(index):
        local = oslquery.OSLQuery()
        assert local.open_bytecode(bytecode)
        assert inspect(local) == expected
        return index

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        assert list(pool.map(inspect_thread, range(32))) == list(range(32))
    report = dict(version=oslquery.VERSION_STRING, originalPackageWrapper=True,
                  sharedOIIOTypeDesc=True, unicodeFilename=True,
                  fileAndMemoryEqual=True, searchPath=True,
                  invalidBytecodeRejected=True, missingFileDiagnostic=True,
                  diagnosticReadAndClear=True, decoderRecovery=True,
                  invalidParameterLookupsRejected=True, threadedQueries=32,
                  query=expected)
    (directory / 'osl-python.json').write_text(json.dumps(report, indent=2) + '\n')
    return report
