package org.unixnu.eldensvc;

final class CdcDescriptors {
    static int dataInterface(byte[] descriptors, int controlId) {
        if (descriptors == null) return -1;
        boolean matchingControl = false;
        for (int offset = 0; offset + 2 <= descriptors.length;) {
            int length = descriptors[offset] & 0xff;
            int type = descriptors[offset + 1] & 0xff;
            if (length < 2 || length > descriptors.length - offset) return -1;
            if (type == 2) {
                matchingControl = false;
            } else if (type == 4) {
                matchingControl = length >= 9 && (descriptors[offset + 2] & 0xff) == controlId
                        && descriptors[offset + 5] == 2 && descriptors[offset + 6] == 2;
            } else if (matchingControl && type == 0x24 && length >= 5
                    && descriptors[offset + 2] == 6 && (descriptors[offset + 3] & 0xff) == controlId) {
                return descriptors[offset + 4] & 0xff;
            }
            offset += length;
        }
        return -1;
    }

    private CdcDescriptors() {}
}
