/*
 * vrtrack_serial.cpp -- an i-glasses! or CyberMaxx head tracker on a DOSBox
 * serial port, for games that read those instead of the VFX1's driver.
 *
 *   [serial]
 *   serial2=iglasses        (or cybermaxx)
 *
 * The tracker itself is libvrtrack (include/vfx/vrtrack.h); this is only the
 * UART side, modelled on DOSBox's dummy serial device. Head orientation is
 * the one the host passes to vfx1Pose(), so every headset sees the same head.
 */
#include "dosbox.h"
#include "setup.h"
#include "serialport.h"

#include "../../ints/vfx1/vfx/vrtrack.h"

void VFX1_GetHead(Bit16s head[3]);          /* vfx1_dosbox.cpp */
double VFX1_TrackerGain(void);
int VFX1_SerialTracker(unsigned index);

#define VRTRACK_RX_EVENT SERIAL_BASE_EVENT_COUNT + 1

class CSerialVrTrack : public CSerial {
public:
	CSerialVrTrack(Bitu id, CommandLine *cmd, VrTrackKind kind) : CSerial(id, cmd), rx_pending(false) {
		vrtrack_init(&track, kind);
		CSerial::Init_Registers();
		setRI(false);
		setCD(true);
		setDSR(true);
		setCTS(true);
		InstallationSuccessful = true;
		LOG_MSG("VFX1: %s head tracker on COM%u", kind == VRTRACK_CYBERMAXX ? "CyberMaxx" : "i-glasses",
		        (unsigned)(id + 1));
	}

	~CSerialVrTrack() {
		removeEvent(SERIAL_TX_EVENT);
		removeEvent(VRTRACK_RX_EVENT);
	}

	void handleUpperEvent(Bit16u type) {
		if (type == SERIAL_TX_EVENT) {
			ByteTransmitted();
		} else if (type == SERIAL_THR_EVENT) {
			ByteTransmitting();
			setEvent(SERIAL_TX_EVENT, bytetime);
		} else if (type == VRTRACK_RX_EVENT) {
			rx_pending = false;
			deliver();
		}
	}

	void transmitByte(Bit8u val, bool first) {
		Bit16s raw[3];
		int16_t head[3];
		VFX1_GetHead(raw);
		vrtrack_scale_head(raw, VFX1_TrackerGain(), head);
		Bit32u before = track.packets;
		vrtrack_write(&track, val, head);
		if (before == 0 && track.packets == 1)
			LOG_MSG("VFX1: first head tracker packet, after %u commands", (unsigned)track.commands);
		if (first) setEvent(SERIAL_THR_EVENT, bytetime / 10);
		else setEvent(SERIAL_TX_EVENT, bytetime);
		schedule();
	}

	void updatePortConfig(Bit16u, Bit8u) {}
	void updateMSR() {}
	void setBreak(bool) {}
	void setRTSDTR(bool rts, bool dtr) { setRTS(rts); setDTR(dtr); }
	void setRTS(bool) {}
	void setDTR(bool) {}

private:
	/* Answer bytes go out as fast as the UART takes them. At the port's own
	 * byte time a six-byte CyberMaxx packet can outlast the 10 ms Descent 2
	 * waits, and it then asks again mid-packet. */
	void schedule() {
		if (!rx_pending && track.out_count) {
			rx_pending = true;
			setEvent(VRTRACK_RX_EVENT, 0.05f);
		}
	}

	void deliver() {
		Bit8u b;
		if (track.out_count && CanReceiveByte() && vrtrack_read(&track, &b)) receiveByte(b);
		schedule();
	}

	VrTrack track;
	bool rx_pending;
};

CSerial *VRTRACK_CreateSerial(Bitu id, CommandLine *cmd, const std::string &type) {
	int host = VFX1_SerialTracker((unsigned)id);          /* the host's choice wins */
	if (host == 0) return new CSerialVrTrack(id, cmd, VRTRACK_IGLASSES);
	if (host == 1) return new CSerialVrTrack(id, cmd, VRTRACK_CYBERMAXX);
	if (type == "iglasses") return new CSerialVrTrack(id, cmd, VRTRACK_IGLASSES);
	if (type == "cybermaxx") return new CSerialVrTrack(id, cmd, VRTRACK_CYBERMAXX);
	return NULL;
}
