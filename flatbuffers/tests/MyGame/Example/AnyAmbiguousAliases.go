// Code generated by the FlatBuffers compiler. DO NOT EDIT.

package Example

import (
	"strconv"

	flatbuffers "github.com/google/flatbuffers/go"
)

type AnyAmbiguousAliases byte

const (
	AnyAmbiguousAliasesNONE AnyAmbiguousAliases = 0
	AnyAmbiguousAliasesM1   AnyAmbiguousAliases = 1
	AnyAmbiguousAliasesM2   AnyAmbiguousAliases = 2
	AnyAmbiguousAliasesM3   AnyAmbiguousAliases = 3
)

var EnumNamesAnyAmbiguousAliases = map[AnyAmbiguousAliases]string{
	AnyAmbiguousAliasesNONE: "NONE",
	AnyAmbiguousAliasesM1:   "M1",
	AnyAmbiguousAliasesM2:   "M2",
	AnyAmbiguousAliasesM3:   "M3",
}

var EnumValuesAnyAmbiguousAliases = map[string]AnyAmbiguousAliases{
	"NONE": AnyAmbiguousAliasesNONE,
	"M1":   AnyAmbiguousAliasesM1,
	"M2":   AnyAmbiguousAliasesM2,
	"M3":   AnyAmbiguousAliasesM3,
}

func (v AnyAmbiguousAliases) String() string {
	if s, ok := EnumNamesAnyAmbiguousAliases[v]; ok {
		return s
	}
	return "AnyAmbiguousAliases(" + strconv.FormatInt(int64(v), 10) + ")"
}

type AnyAmbiguousAliasesT struct {
	Type AnyAmbiguousAliases
	Value interface{}
}

func AnyAmbiguousAliasesPack(builder *flatbuffers.Builder, t *AnyAmbiguousAliasesT) flatbuffers.UOffsetT {
	if t == nil {
		return 0
	}
	switch t.Type {
	case AnyAmbiguousAliasesM1:
		return MonsterPack(builder, t.Value.(*MonsterT))
	case AnyAmbiguousAliasesM2:
		return MonsterPack(builder, t.Value.(*MonsterT))
	case AnyAmbiguousAliasesM3:
		return MonsterPack(builder, t.Value.(*MonsterT))
	}
	return 0
}

func AnyAmbiguousAliasesUnPack(t AnyAmbiguousAliases, table flatbuffers.Table) *AnyAmbiguousAliasesT {
	switch t {
	case AnyAmbiguousAliasesM1:
		x := Monster{_tab: table}
		return &AnyAmbiguousAliasesT{ Type: AnyAmbiguousAliasesM1, Value: x.UnPack() }
	case AnyAmbiguousAliasesM2:
		x := Monster{_tab: table}
		return &AnyAmbiguousAliasesT{ Type: AnyAmbiguousAliasesM2, Value: x.UnPack() }
	case AnyAmbiguousAliasesM3:
		x := Monster{_tab: table}
		return &AnyAmbiguousAliasesT{ Type: AnyAmbiguousAliasesM3, Value: x.UnPack() }
	}
	return nil
}