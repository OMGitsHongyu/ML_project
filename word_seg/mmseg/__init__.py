import os
from ._mmseg import Dictionary as _Dictionary, Token, Algorithm

__all__ = ('Dictionary', 'Token', 'Algorithm', 'dict_load_defaults')

class Dictionary(_Dictionary):
    dictionaries = (
        ('chars', os.path.join(os.path.dirname(__file__), 'chars.dic')),
        ('words', os.path.join(os.path.dirname(__file__), 'words.dic')),
    )

    @staticmethod
    def load_dictionaries():
        for t, d in Dictionary.dictionaries:
            if t == 'chars':
                if not Dictionary.load_chars(d):
                    raise IOError("Cannot open '%s'" % d)
            elif t == 'words':
                if not Dictionary.load_words(d):
                    raise IOError("Cannot open '%s'" % d)

dict_load_defaults = Dictionary.load_dictionaries
