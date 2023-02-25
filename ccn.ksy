meta:
  id: ccn
  file-extension: ccn
  endian: le
seq:
  - id: frame
    type: frame
types:
  frame:
    seq:
      - id: dstclass
        type: u1
      - id: dstaddress
        type: u1
      - id: txclass
        type: u1
      - id: txaddress
        type: u1
      - id: len
        type: u1
      - id: pid
        type: u1
      - id: ext
        type: u1
      - id: function
        type: u1
        enum: func_type
      - id: data
        type: str
        size: len
        encoding: UTF-8
      - id: checksum
        type: u2
    enums:
      func_type:
        0x06: reply
        0x0B: read
        0x0C: write
        0x15: exception
