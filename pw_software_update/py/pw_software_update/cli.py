# Copyright 2022 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""
Software update related operations.

Learn more at: pigweed.dev/pw_software_update

"""

import argparse
import sys
from pathlib import Path
from pw_software_update import dev_sign, keys, root_metadata
from pw_software_update.tuf_pb2 import SignedRootMetadata


def sign_root_metadata_handler(arg) -> None:
    """Handler for signing of root metadata"""
    try:
        signed_root_metadata = dev_sign.sign_root_metadata(
            SignedRootMetadata.FromString(arg.root_metadata.read_bytes()),
            arg.root_key.read_bytes())
        arg.root_metadata.write_bytes(signed_root_metadata.SerializeToString())

    except IOError as error:
        print(error)


def _add_sign_root_metadata_parser(subparsers) -> None:
    """Parser to handle sign-root-metadata subcommand"""

    formatter_class = lambda prog: argparse.HelpFormatter(
        prog, max_help_position=100, width=200)
    sign_root_metadata_parser = subparsers.add_parser(
        'sign-root-metadata',
        description='Signing of root metadata',
        formatter_class=formatter_class,
        help="",
    )

    sign_root_metadata_parser.set_defaults(func=sign_root_metadata_handler)
    required_arguments = sign_root_metadata_parser.add_argument_group(
        'required arguments')
    required_arguments.add_argument('--root-metadata',
                                    help='Root metadata to be signed',
                                    metavar='ROOT_METADATA',
                                    required=True,
                                    type=Path)
    required_arguments.add_argument('--root-key',
                                    help='Root signing key',
                                    metavar='ROOT_KEY',
                                    required=True,
                                    type=Path)


def create_root_metadata_handler(arg) -> None:
    """Handler function for creation of root metadata."""
    try:
        root_metadata.main(arg.out, arg.append_root_key,
                           arg.append_targets_key, arg.version)

        # TODO(eashansingh): Print message that allows user
        # to visualize root metadata with
        # `pw update inspect-root-metadata` command

    except IOError as error:
        print(error)


def _add_create_root_metadata_parser(subparsers) -> None:
    """Parser to handle create-root-metadata subcommand."""

    formatter_class = lambda prog: argparse.HelpFormatter(
        prog, max_help_position=100, width=200)
    create_root_metadata_parser = subparsers.add_parser(
        'create-root-metadata',
        description='Creation of root metadata',
        formatter_class=formatter_class,
        help='',
    )
    create_root_metadata_parser.set_defaults(func=create_root_metadata_handler)
    create_root_metadata_parser.add_argument(
        '--version',
        help='Canonical version number for rollback checks; Defaults to 1',
        type=int,
        default=1,
        required=False)

    required_arguments = create_root_metadata_parser.add_argument_group(
        'required arguments')
    required_arguments.add_argument('--append-root-key',
                                    help='Path to root key',
                                    metavar='ROOT_KEY',
                                    required=True,
                                    action='append',
                                    type=Path)
    required_arguments.add_argument('--append-targets-key',
                                    help='Path to targets key',
                                    metavar='TARGETS_KEY',
                                    required=True,
                                    action='append',
                                    type=Path)
    required_arguments.add_argument('-o',
                                    '--out',
                                    help='Path to output file',
                                    required=True,
                                    type=Path)


def generate_key_handler(arg) -> None:
    """ Handler function for key generation"""

    try:
        keys.gen_ecdsa_keypair(arg.pathname)
        print('Private key: ' + str(arg.pathname))
        print('Public key: ' + str(arg.pathname) + '.pub')

    except IOError as error:
        print(error)


def _add_generate_key_parser(subparsers) -> None:
    """Parser to handle key generation subcommand."""

    generate_key_parser = subparsers.add_parser(
        'generate-key',
        description=
        'Generates an ecdsa-sha2-nistp256 signing key pair (private + public)',
        help='')
    generate_key_parser.set_defaults(func=generate_key_handler)
    generate_key_parser.add_argument('pathname',
                                     type=Path,
                                     help='Path to generated key pair')


def _parse_args() -> argparse.Namespace:
    parser_root = argparse.ArgumentParser(
        description='Software update related operations.',
        epilog='Learn more at: pigweed.dev/pw_software_update')
    parser_root.set_defaults(
        func=lambda *_args, **_kwargs: parser_root.print_help())

    subparsers = parser_root.add_subparsers()
    _add_generate_key_parser(subparsers)
    _add_create_root_metadata_parser(subparsers)
    _add_sign_root_metadata_parser(subparsers)

    return parser_root.parse_args()


def _dispatch_command(args) -> None:
    args.func(args)


def main() -> int:
    """Software update command-line interface(WIP)."""
    _dispatch_command(_parse_args())
    return 0


if __name__ == '__main__':
    sys.exit(main())
