# Client-side resource classes for the nucula rig env (labgrid-env.yaml
# imports this). BenchSerialToken mirrors the microfips bench class —
# the tokens are acquisition/exclusivity records only; direct device
# access (pcscd, serial) stays the working path.
import attr

from labgrid.resource.common import Resource


@attr.s(eq=False)
class BenchSerialToken(Resource):
    """A bench board's serial interface as a coordinator-visible token."""

    usb_serial = attr.ib(default="", validator=attr.validators.instance_of(str))
    vidpid = attr.ib(default="", validator=attr.validators.instance_of(str))
    id_path = attr.ib(default="", validator=attr.validators.instance_of(str))
